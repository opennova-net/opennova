// The indoor-visibility walk on the retail data (the "occlusion issues in a
// building" report): boot a retail mission on the headless rig, pick an
// enterable building with blink boxes and portal records, walk the local
// player from outside through its doorway into the interior and back out, and
// after every logic tick run the render-occlusion frame over the camera the
// game draws with (world::LocalPlayer::view_frame -> OcclusionFrameCamera),
// recording per step:
//   * the local player's blink letters (the frame gates' input) and the
//     scene-pass gates they drive (terrain / sky);
//   * the camera blink query, camera_indoors / exterior_visible, the target
//     building's raw section mask and batch verdict;
//   * the entity collector's verdict for every non-building entity and the
//     batch verdict of every building.
// Symptom (B) "the outside flickers crossing the doorway": a gate or a
// visibility verdict toggling more than once over a monotonic walk.
// Symptom (A) "things inside can't be seen from inside": an entity inside
// the building culled from inside while it draws from outside looking in.
// Gated on OPENNOVA_JO_DIR (a retail JO install carrying the mission).
//
// Options: --bms <file> (default 00TRa.bms), --list (print the buildings),
// --bms-id <id> (the building), --door <record> (its type-2 record),
// --tp (the debug on-foot chase camera; default the on-foot first person),
// --route x,y;x,y;... (walk these mission waypoints after the door legs),
// --span <u> / --inside <u> (how far outside / inside the door plane the legs
// run), --verbose (one line per tick).
#include "common/retail_mission_files.h"
#include "common/retail_paths.h"

#include <runtime/mission/collision_resolve.h>
#include <runtime/mission/placement_traits.h>
#include <runtime/renderer/scene_pass_gates.h>
#include <runtime/world/angle.h>
#include <runtime/world/collision.h>
#include <runtime/world/entity_pose.h>
#include <runtime/world/local_player_view.h>
#include <runtime/world/occlusion.h>
#include <runtime/world/occlusion_camera.h>
#include <runtime/world/player_view.h>
#include <runtime/world/presentation_frame.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace {

using namespace opennova;
namespace w = opennova::world;

constexpr double kPi = 3.14159265358979323846;

int failures = 0;
std::vector<int> g_watch; // --watch <bms>
bool g_occluders = false; // --occluders: the target's type-0/1 record probe rows
bool g_slots = false;     // --slots: the replicated slot list + the watched verdicts
opennova::world::OcclusionFrameCamera g_last_cam; // the last run_frame camera (the --why dump)
bool expect(bool cond, const char *msg) {
	if (cond) return true;
	std::fprintf(stderr, "FAIL: %s\n", msg);
	++failures;
	return false;
}

struct Options {
	std::string bms = "00TRa.bms";
	bool list = false;
	bool scan = false;
	bool spawn = false;   // --spawn: the authored spawn's verdicts over a look-around
	float radius = 40.0f; // --radius: the spawn listing range
	int yaw_step = 15;    // --yaw-step: the look-around step, degrees
	int idle = 0;         // --idle N: stand still at the spawn for N ticks first
	bool why = false;     // --why: the spawn facing's per-entity verdicts
	bool eye = false;     // --eye: the FP eye vs retail's composition chain at pitch -20/0/+20
	bool retail_truth = false; // --retail-truth: assert what retail draws at the CP03 spawn poses
	float sweep_step = 0.0f;   // --sweep-out <u>: settled static poses out through the door (no walk)
	int bms_id = 0;
	int door = -1;
	bool third_person = false;
	std::vector<int> watch; // --watch <bms>: per-frame batch/TOC/latch rows
	bool verbose = false;
	float span = 5.0f;   // the legs start / end this far outside the door plane
	float inside = 2.5f; // the in leg stops this far inside it
	std::vector<std::pair<float, float>> route;
};

// Mission (x, y, z) from the render float world (X, Y, Z) = (-y, z, x).
void mission_from_render(const float r[3], float out[3]) {
	out[0] = r[2];
	out[1] = -r[0];
	out[2] = r[1];
}

std::string item_label(testrig::RetailMissionRig &rig, const w::Entity &e) {
	const def::DefItemsFile *items = rig.items_table();
	if (items == nullptr) return "?";
	const def::DefItemDef *d = mission::find_item_def(
			*items, mission::visual_item_id_for_runtime_type(e.item_id, *items));
	if (d == nullptr) return "?";
	return std::string(d->sid) + "/" + d->graphic;
}

std::string hit_str(uint32_t h) {
	if (h == 0) return "-";
	char buf[32];
	std::snprintf(buf, sizeof(buf), "s%d:%d", w::BlinkAccum::hit_pool_entity_index(h),
			w::BlinkAccum::hit_section(h));
	return buf;
}

struct DoorRecord {
	int index = -1;
	int type = 0;
	int section_a = 0, section_b = 0;
	float center[3] = {}; // mission
	float normal[3] = {}; // mission, unit, the first face's plane
	float min_z = 0.0f, max_z = 0.0f;
};

std::vector<DoorRecord> portal_records(testrig::RetailMissionRig &rig, const w::Entity &e) {
	std::vector<DoorRecord> out;
	const w::OcclusionModel *m = rig.occlusion.model(rig.occlusion.instance_model_id(e.handle));
	if (m == nullptr) return out;
	const w::RenderMatrix mat = w::render_matrix_from_entity_pose(e);
	for (size_t r = 0; r < m->records.size(); ++r) {
		const w::OcclusionPortalFace &rec = m->records[r];
		DoorRecord d;
		d.index = static_cast<int>(r);
		d.type = rec.type;
		d.section_a = rec.section_a;
		d.section_b = rec.section_b;
		float rw[3];
		mat.transform_point(rec.pos, rw);
		mission_from_render(rw, d.center);
		d.min_z = 1e30f;
		d.max_z = -1e30f;
		for (int32_t v = 0; v < rec.vert_count; ++v) {
			float vw[3], vm[3];
			mat.transform_point(m->vertices[rec.vert_start + v].p, vw);
			mission_from_render(vw, vm);
			d.min_z = std::min(d.min_z, vm[2]);
			d.max_z = std::max(d.max_z, vm[2]);
		}
		if (rec.face_count > 0) {
			const w::OcclusionFaceRec &f0 = m->faces[rec.face_start];
			float nw[3], nm[3];
			mat.rotate_vector(m->planes[rec.plane_start + f0.plane].normal, nw);
			mission_from_render(nw, nm);
			const float len = std::sqrt(nm[0] * nm[0] + nm[1] * nm[1] + nm[2] * nm[2]);
			if (len > 0.0f)
				for (int i = 0; i < 3; ++i) d.normal[i] = nm[i] / len;
		}
		out.push_back(d);
	}
	return out;
}

w::BlinkAccum point_query(testrig::RetailMissionRig &rig, const float p[3]) {
	const int32_t pf[3] = {w::to_fixed(p[0]), w::to_fixed(p[1]), w::to_fixed(p[2])};
	w::BlinkAccum acc;
	rig.collision.query_blink_boxes_at_point(rig.world, pf, acc);
	return acc;
}

void list_buildings(testrig::RetailMissionRig &rig) {
	const w::Vec3 spawn = rig.local.player_position();
	rig.world.registry.for_each([&](const w::Entity &e) {
		if (e.kind != w::EntityKind::Building) return;
		if (!rig.occlusion.has_instance(e.handle)) return;
		const w::OcclusionWorld::BuildingFlags fl = rig.occlusion.building_flags(e.handle);
		const std::vector<DoorRecord> recs = portal_records(rig, e);
		int counts[8] = {};
		for (const DoorRecord &d : recs) counts[std::min(d.type, 7)]++;
		std::printf("bldg bms=%d slot=%d %s pos (%.1f, %.1f, %.1f) yaw %d dist %.0f  recs t0=%d t1=%d t2=%d t3=%d t5=%d  win=%d open=%d link=%d\n",
				e.bms_id, e.handle.slot(), item_label(rig, e).c_str(), e.position.x, e.position.y,
				e.position.z, static_cast<int>(e.yaw), testrig::planar_distance(e.position, spawn),
				counts[0], counts[1], counts[2], counts[3], counts[5], fl.has_windows, fl.has_open,
				fl.has_links);
		for (const DoorRecord &d : recs) {
			if (d.type != w::kOccRecWindow && d.type != w::kOccRecPortal) continue;
			std::printf("    rec %d type %d A=%d B=%d at (%.2f, %.2f, %.2f) z %.2f..%.2f n (%.2f, %.2f, %.2f)\n",
					d.index, d.type, d.section_a, d.section_b, d.center[0], d.center[1], d.center[2],
					d.min_z, d.max_z, d.normal[0], d.normal[1], d.normal[2]);
		}
		const w::CollisionModel *cm = rig.collision.model_for(rig.world, e.handle);
		if (cm == nullptr) return;
		for (size_t si = 0; si < cm->sections.size(); ++si) {
			const w::CollisionSection &sec = cm->sections[si];
			for (int32_t vi = 0; vi < sec.volume_count; ++vi) {
				const w::CollisionVolume &v = cm->volumes[sec.volume_start + vi];
				if (v.type != 8) continue;
				std::printf("    BB section %zu flags 0x%02X accum 0x%02X local x %.2f..%.2f y %.2f..%.2f z %.2f..%.2f\n",
						si, v.flags, v.flags ^ 6u, v.min_x / 65536.0, v.max_x / 65536.0,
						v.min_y / 65536.0, v.max_y / 65536.0, v.min_z / 65536.0, v.max_z / 65536.0);
			}
		}
	});
}

struct Frame {
	int tick = 0;
	float pos[3] = {};
	float eye[3] = {};
	float yaw = 0.0f;
	float along = 0.0f; // signed distance past the door plane, + = inside
	uint32_t letters = 0;
	uint32_t player_hit0 = 0;
	uint32_t camera_hit0 = 0;
	uint32_t camera_flags = 0;
	bool camera_indoors = false;
	bool exterior_visible = false;
	bool water_visible = false;
	bool bldg_batched = false;
	bool bldg_visible = false;
	uint32_t bldg_mask = 0;
	int window_groups = 0;
	int visible_buildings = 0;
	renderer::ScenePassGates gates;
	std::set<int> culled;           // bms ids the collector culled
	std::set<int> hidden_buildings; // bms ids of buildings not drawn
	std::string watch;              // the --watch rows
	std::string occluders;          // the target building's type-0/1 records vs the camera
	std::string slots;              // the frame's replicated slot list and the watched verdicts
};

// The frame's portal-slot list replicated from the public verdicts
// (OcclusionWorld::collect_buildings + sort_portal_slots): batched buildings
// in static-table order within 250 u per axis, their type-0/1 records in the
// frustum with radius^2/dist^2 > 0.01, 128 cap, then the first 14 (JO's slot
// weights are zero, so the stable sort keeps collection order). For each
// watched entity, the slots whose silhouette fully holds its sphere (the
// render_TOC slot leg before the 8-corner refinement and the viewthru rescue;
// a type-1 slot counts only while its building carries the bit-31 marker).
std::string slot_probe(testrig::RetailMissionRig &rig, const w::OcclusionFrameCamera &cam) {
	struct Slot {
		w::EntityHandle h;
		int bms = 0;
		size_t rec = 0;
		int type = 0;
		std::vector<std::array<float, 4>> eplanes, fplanes;
	};
	std::vector<Slot> slots;
	const int32_t buildings = rig.collision.static_building_count();
	for (int32_t i = 0; i < buildings && slots.size() < 128; ++i) {
		const w::CollisionWorld::StaticSlotView sv = rig.collision.static_slot(i);
		if (!rig.occlusion.building_batched(sv.h)) continue;
		const int32_t dx = std::abs(w::static_slot_coord_q16(sv.x) - cam.pos_fixed[0]);
		const int32_t dy = std::abs(w::static_slot_coord_q16(sv.y) - cam.pos_fixed[1]);
		if (dx >= (250 << 16) || dy >= (250 << 16)) continue;
		const w::OcclusionModel *m = rig.occlusion.model(rig.occlusion.instance_model_id(sv.h));
		const w::Entity *be = rig.world.registry.get(sv.h);
		if (m == nullptr || be == nullptr) continue;
		const w::RenderMatrix mat = w::render_matrix_from_entity_pose(*be);
		for (size_t r = 0; r < m->records.size() && slots.size() < 128; ++r) {
			const w::OcclusionPortalFace &rec = m->records[r];
			if (rec.type != w::kOccRecOccluder && rec.type != w::kOccRecOpen) continue;
			float rw[3];
			mat.transform_point(rec.pos, rw);
			bool in_view = true;
			for (int32_t p = 0; p < cam.frustum_count; ++p)
				if (rw[0] * cam.frustum[p][0] + rw[1] * cam.frustum[p][1] + rw[2] * cam.frustum[p][2] +
								cam.frustum[p][3] < -rec.radius)
					in_view = false;
			if (!in_view) continue;
			const float ddx = rw[0] - cam.pos_float[0], ddy = rw[1] - cam.pos_float[1],
					ddz = rw[2] - cam.pos_float[2];
			if (0.01f >= rec.radius * rec.radius / (ddx * ddx + ddy * ddy + ddz * ddz)) continue;
			Slot s;
			s.h = sv.h;
			s.bms = be->bms_id;
			s.rec = r;
			s.type = rec.type;
			std::vector<uint16_t> edges;
			std::vector<int> seen;
			for (int32_t f = 0; f < rec.face_count; ++f) {
				const w::OcclusionFaceRec &face = m->faces[rec.face_start + f];
				float v0[3], n[3];
				mat.transform_point(m->vertices[rec.vert_start + face.v[0]].p, v0);
				mat.rotate_vector(m->planes[rec.plane_start + face.plane].normal, n);
				const float d = n[0] * (v0[0] - cam.pos_float[0]) + n[1] * (v0[1] - cam.pos_float[1]) +
						n[2] * (v0[2] - cam.pos_float[2]);
				if (d > 0.0f) {
					if (std::find(seen.begin(), seen.end(), face.plane) == seen.end()) {
						seen.push_back(face.plane);
						s.fplanes.push_back({n[0], n[1], n[2], -(n[0] * v0[0] + n[1] * v0[1] + n[2] * v0[2])});
					}
					continue;
				}
				for (int k = 0; k < 3; ++k) {
					const uint16_t e = face.edge[k];
					auto it = std::find_if(edges.begin(), edges.end(),
							[&](uint16_t x) { return (x & 0x7FFF) == (e & 0x7FFF); });
					if (it != edges.end()) {
						*it = edges.back();
						edges.pop_back();
					} else {
						edges.push_back(e);
					}
				}
			}
			for (const uint16_t e : edges) {
				float a[3], b[3];
				mat.transform_point(m->vertices[rec.vert_start + (e & 0xFF)].p, a);
				mat.transform_point(m->vertices[rec.vert_start + ((e >> 8) & 0x7F)].p, b);
				const float ex = b[0] - a[0], ey = b[1] - a[1], ez = b[2] - a[2];
				const float cx = cam.pos_float[0] - a[0], cy = cam.pos_float[1] - a[1],
						cz = cam.pos_float[2] - a[2];
				float nx, ny, nz;
				if ((e & 0x8000) != 0) {
					nx = cz * ey - cy * ez; ny = cx * ez - cz * ex; nz = cy * ex - cx * ey;
				} else {
					nx = cy * ez - cz * ey; ny = cz * ex - cx * ez; nz = cx * ey - cy * ex;
				}
				const float len = std::sqrt(nx * nx + ny * ny + nz * nz);
				if (len > 0.0f) { nx /= len; ny /= len; nz /= len; }
				s.eplanes.push_back({nx, ny, nz, -(nx * a[0] + ny * a[1] + nz * a[2])});
			}
			slots.push_back(std::move(s));
		}
	}
	if (slots.size() > 14) slots.resize(14);
	std::string out;
	char buf[96];
	std::snprintf(buf, sizeof(buf), " slots=%zu:", slots.size());
	out += buf;
	for (const Slot &s : slots) {
		std::snprintf(buf, sizeof(buf), " %d.r%zu%s", s.bms, s.rec, s.type == 1 ? "o" : "");
		out += buf;
	}
	for (int id : g_watch) {
		rig.world.registry.for_each([&](const w::Entity &e) {
			if (e.bms_id != id) return;
			int32_t pf[3] = {w::to_fixed(e.position.x), w::to_fixed(e.position.y), w::to_fixed(e.position.z)};
			float pos[3];
			w::render_float_from_fixed(pf, pos);
			const float radius = e.bound_radius;
			std::string by;
			for (const Slot &s : slots) {
				if (s.h == e.handle) continue;
				if (s.type == 1 && (rig.occlusion.section_mask(s.h) & 0x80000000u) == 0) continue;
				if (s.fplanes.empty()) continue;
				bool beyond = false;
				for (const auto &pl : s.fplanes)
					if (pos[0] * pl[0] + pos[1] * pl[1] + pos[2] * pl[2] + pl[3] > 0.0f) beyond = true;
				if (!beyond) continue;
				bool fully = true, partial = false;
				for (const auto &pl : s.eplanes) {
					const float d = pos[0] * pl[0] + pos[1] * pl[1] + pos[2] * pl[2] + pl[3];
					if (d < 0.0f) partial = true;
					if (d > -radius) { fully = false; break; }
				}
				if (fully || partial) {
					std::snprintf(buf, sizeof(buf), " %s%d.r%zu", fully ? "FULL:" : "part:", s.bms, s.rec);
					by += buf;
				}
			}
			std::snprintf(buf, sizeof(buf), " {%d:%s }", id, by.c_str());
			out += buf;
		});
	}
	return out;
}

// The target building's occluder records (types 0/1) against this frame's
// camera: the collection gates (frustum sphere, radius^2/dist^2 > 0.01) and
// the silhouette build's classification (front = n.(v0 - eye) > 0 per face;
// back faces feed the cancelled edge list) — a camera inside a closed hull
// sees every face front, so the record yields face planes and no edge planes.
std::string occluder_probe(testrig::RetailMissionRig &rig, const w::Entity &bldg,
		const w::OcclusionFrameCamera &cam) {
	const w::OcclusionModel *m = rig.occlusion.model(rig.occlusion.instance_model_id(bldg.handle));
	if (m == nullptr) return "";
	const w::RenderMatrix mat = w::render_matrix_from_entity_pose(bldg);
	std::string out;
	for (size_t r = 0; r < m->records.size(); ++r) {
		const w::OcclusionPortalFace &rec = m->records[r];
		if (rec.type != w::kOccRecOccluder && rec.type != w::kOccRecOpen) continue;
		float rw[3];
		mat.transform_point(rec.pos, rw);
		bool in_view = true;
		for (int32_t p = 0; p < cam.frustum_count; ++p) {
			const float d = rw[0] * cam.frustum[p][0] + rw[1] * cam.frustum[p][1] +
					rw[2] * cam.frustum[p][2] + cam.frustum[p][3];
			if (d < -rec.radius) in_view = false;
		}
		const float dx = rw[0] - cam.pos_float[0], dy = rw[1] - cam.pos_float[1],
				dz = rw[2] - cam.pos_float[2];
		const bool big = rec.radius * rec.radius / (dx * dx + dy * dy + dz * dz) > 0.01f;
		int fronts = 0, backs = 0;
		std::vector<uint16_t> edges;
		for (int32_t f = 0; f < rec.face_count; ++f) {
			const w::OcclusionFaceRec &face = m->faces[rec.face_start + f];
			float v0[3], n[3];
			mat.transform_point(m->vertices[rec.vert_start + face.v[0]].p, v0);
			mat.rotate_vector(m->planes[rec.plane_start + face.plane].normal, n);
			const float d = n[0] * (v0[0] - cam.pos_float[0]) + n[1] * (v0[1] - cam.pos_float[1]) +
					n[2] * (v0[2] - cam.pos_float[2]);
			if (d > 0.0f) {
				++fronts;
				continue;
			}
			++backs;
			for (int k = 0; k < 3; ++k) {
				const uint16_t e = face.edge[k];
				auto it = std::find_if(edges.begin(), edges.end(),
						[&](uint16_t x) { return (x & 0x7FFF) == (e & 0x7FFF); });
				if (it != edges.end()) {
					*it = edges.back();
					edges.pop_back();
				} else {
					edges.push_back(e);
				}
			}
		}
		// The occluder planes the slot would carry (OcclusionWorld::
		// build_occluder_planes / Terrain_BuildClipPlanesFromCollision): the
		// silhouette edge planes (winding set: e x c, clear: c x e) and the
		// front-face planes deduped by plane index.
		std::vector<std::array<float, 4>> eplanes, fplanes;
		std::vector<int> seen_planes;
		for (const uint16_t e : edges) {
			float a[3], b[3];
			mat.transform_point(m->vertices[rec.vert_start + (e & 0xFF)].p, a);
			mat.transform_point(m->vertices[rec.vert_start + ((e >> 8) & 0x7F)].p, b);
			const float ex = b[0] - a[0], ey = b[1] - a[1], ez = b[2] - a[2];
			const float cx = cam.pos_float[0] - a[0], cy = cam.pos_float[1] - a[1],
					cz = cam.pos_float[2] - a[2];
			float nx, ny, nz;
			if ((e & 0x8000) != 0) {
				nx = cz * ey - cy * ez; ny = cx * ez - cz * ex; nz = cy * ex - cx * ey;
			} else {
				nx = cy * ez - cz * ey; ny = cz * ex - cx * ez; nz = cx * ey - cy * ex;
			}
			const float len = std::sqrt(nx * nx + ny * ny + nz * nz);
			if (len > 0.0f) { nx /= len; ny /= len; nz /= len; }
			eplanes.push_back({nx, ny, nz, -(nx * a[0] + ny * a[1] + nz * a[2])});
		}
		for (int32_t f = 0; f < rec.face_count; ++f) {
			const w::OcclusionFaceRec &face = m->faces[rec.face_start + f];
			float v0[3], n[3];
			mat.transform_point(m->vertices[rec.vert_start + face.v[0]].p, v0);
			mat.rotate_vector(m->planes[rec.plane_start + face.plane].normal, n);
			const float d = n[0] * (v0[0] - cam.pos_float[0]) + n[1] * (v0[1] - cam.pos_float[1]) +
					n[2] * (v0[2] - cam.pos_float[2]);
			if (d <= 0.0f) continue;
			if (std::find(seen_planes.begin(), seen_planes.end(), face.plane) != seen_planes.end()) continue;
			seen_planes.push_back(face.plane);
			fplanes.push_back({n[0], n[1], n[2], -(n[0] * v0[0] + n[1] * v0[1] + n[2] * v0[2])});
		}
		// The record's own centre sits behind its silhouette: count the edge
		// planes that hold it inside (all of them for a sane wedge).
		int center_in = 0;
		for (const auto &pl : eplanes)
			if (rw[0] * pl[0] + rw[1] * pl[1] + rw[2] * pl[2] + pl[3] >= 0.0f) ++center_in;
		char buf[160];
		std::snprintf(buf, sizeof(buf), " [r%zu t%d r=%.1f view=%d big=%d front=%d back=%d edges=%zu ctr_in=%d]", r,
				rec.type, rec.radius, in_view, big, fronts, backs, edges.size(), center_in);
		out += buf;
		// Each watched entity against this record (render_TOC's slot legs,
		// before the 8-corner refinement and the viewthru rescue).
		for (int id : g_watch) {
			rig.world.registry.for_each([&](const w::Entity &e) {
				if (e.bms_id != id) return;
				int32_t pf[3] = {w::to_fixed(e.position.x), w::to_fixed(e.position.y),
						w::to_fixed(e.position.z)};
				float pos[3];
				w::render_float_from_fixed(pf, pos);
				const float radius = e.bound_radius;
				bool beyond = false;
				for (const auto &pl : fplanes)
					if (pos[0] * pl[0] + pos[1] * pl[1] + pos[2] * pl[2] + pl[3] > 0.0f) beyond = true;
				bool fully = true, partial = false;
				for (const auto &pl : eplanes) {
					const float d = pos[0] * pl[0] + pos[1] * pl[1] + pos[2] * pl[2] + pl[3];
					if (d < 0.0f) partial = true;
					if (d > -radius) { fully = false; break; }
				}
				char wb[96];
				std::snprintf(wb, sizeof(wb), "{%d beyond=%d fully=%d partial=%d}", id, beyond, fully, partial);
				out += wb;
			});
		}
	}
	return out;
}

// One render-occlusion frame over the camera the game draws with.
bool run_frame(testrig::RetailMissionRig &rig, const w::Entity &bldg, const DoorRecord &door,
		const float inward[3], Frame &out) {
	const w::LocalPlayerViewFrame f = rig.local.view_frame();
	if (!f.camera_pose_valid) return false;
	w::OcclusionViewSpec view;
	w::presentation_from_mission(f.camera.eye, view.eye);
	// The presenter's camera basis (godot util/axes.h mission_view_transform):
	// the BMS converter's basis turned a half circle about up.
	const mission::PlacementBasis b = mission::bms_to_presentation_basis(
			f.camera.pitch_deg, f.camera.yaw_deg, f.camera.roll_deg);
	view.forward[0] = b.z.x; view.forward[1] = b.z.y; view.forward[2] = b.z.z;
	view.right[0] = -b.x.x; view.right[1] = -b.x.y; view.right[2] = -b.x.z;
	view.up[0] = b.y.x; view.up[1] = b.y.y; view.up[2] = b.y.z;
	const double aspect = 16.0 / 9.0;
	const double fov_h = f.fov_h_deg > 1.0f ? f.fov_h_deg : 90.0;
	const double tan_h = std::tan(fov_h * 0.5 * kPi / 180.0);
	view.fov_y_deg = static_cast<float>(2.0 * std::atan(tan_h / aspect) * 180.0 / kPi);
	view.aspect = static_cast<float>(aspect);
	view.viewport_width = 1600.0f;
	view.fog_dist_units = 600.0f;
	view.water_z_units = rig.world.env.water_z != 0
			? static_cast<float>(rig.world.env.water_z / 65536.0)
			: -100000.0f;
	view.local_blink_flags = rig.collision.local_player_blink_flags;
	w::OcclusionFrameCamera cam;
	w::occlusion_camera_from_view(view, cam);
	g_last_cam = cam;
	rig.occlusion.build_frame(rig.world, rig.collision, cam);

	const w::Vec3 p = rig.local.player_position();
	out.tick = static_cast<int>(rig.world.logic_tick);
	out.pos[0] = p.x; out.pos[1] = p.y; out.pos[2] = p.z;
	for (int i = 0; i < 3; ++i) out.eye[i] = f.camera.eye[i];
	out.yaw = f.camera.yaw_deg;
	out.along = (p.x - door.center[0]) * inward[0] + (p.y - door.center[1]) * inward[1];
	out.letters = rig.collision.local_player_blink_flags;
	if (const w::Entity *pl = rig.local.player()) out.player_hit0 = pl->blink_hits[0];
	const w::BlinkAccum ch = point_query(rig, f.camera.eye);
	out.camera_hit0 = ch.hits[0];
	out.camera_flags = ch.flags;
	out.camera_indoors = rig.occlusion.camera_indoors();
	out.exterior_visible = rig.occlusion.exterior_visible();
	out.water_visible = rig.occlusion.water_visible();
	out.bldg_batched = rig.occlusion.building_batched(bldg.handle);
	out.bldg_visible = rig.occlusion.building_visible(bldg.handle);
	out.bldg_mask = rig.occlusion.section_mask(bldg.handle);
	out.window_groups = rig.occlusion.window_frustum_group_count();
	out.occluders = occluder_probe(rig, bldg, cam);
	if (g_slots) out.slots = slot_probe(rig, cam);
	for (int id : g_watch) {
		rig.world.registry.for_each([&](const w::Entity &e) {
			if (e.bms_id != id) return;
			char buf[160];
			const float dx = e.position.x - f.camera.eye[0], dy = e.position.y - f.camera.eye[1];
			std::snprintf(buf, sizeof(buf), " [%d %s d=%.0f batched=%d vis=%d latch=%d mask=0x%X]", id,
					e.kind == w::EntityKind::Building ? "B" : "E", std::sqrt(dx * dx + dy * dy),
					rig.occlusion.building_batched(e.handle), rig.occlusion.building_visible(e.handle),
					static_cast<int>(e.occlusion_latch), rig.occlusion.section_mask(e.handle));
			out.watch += buf;
		});
	}
	out.gates = renderer::scene_pass_gates(out.letters, false);

	// The collector walk the shell runs (Simulation::run_occlusion_frame) and
	// the building verdicts it applies (get_building_visibility_changes): the
	// statics table's building rows take the batch verdict, every other row
	// (the pool-2 decorations and foliage included) the entity gates.
	std::vector<w::EntityHandle> handles;
	rig.world.registry.for_each([&](const w::Entity &e) {
		if (e.kind == w::EntityKind::Building && w::building_def_row(e)) {
			if (e.bms_id == 0) return;
			if (!rig.occlusion.has_instance(e.handle) &&
					rig.collision.model_for(rig.world, e.handle) == nullptr)
				return;
			if (rig.occlusion.building_visible(e.handle)) ++out.visible_buildings;
			else out.hidden_buildings.insert(e.bms_id);
			return;
		}
		if (e.kind == w::EntityKind::Marker || e.bms_id == 0) return;
		handles.push_back(e.handle);
	});
	for (const w::EntityHandle h : handles) {
		w::Entity *e = rig.world.registry.get(h);
		if (e == nullptr) continue;
		if (!rig.occlusion.entity_render_visible(rig.world, rig.collision, *e, cam))
			out.culled.insert(e->bms_id);
	}
	return true;
}

void print_frame(const Frame &fr) {
	std::printf("t%5d along %+6.2f pos (%.2f,%.2f,%.2f) eye (%.2f,%.2f,%.2f) yaw %6.1f letters 0x%02X plhit %-7s camhit %-7s camfl 0x%02X in=%d ext=%d wat=%d  bldg b=%d v=%d mask 0x%08X wg=%d  terr=%d sky=%d  vis_bldg=%d culled=%zu\n",
			fr.tick, fr.along, fr.pos[0], fr.pos[1], fr.pos[2], fr.eye[0], fr.eye[1], fr.eye[2],
			fr.yaw, fr.letters, hit_str(fr.player_hit0).c_str(), hit_str(fr.camera_hit0).c_str(),
			fr.camera_flags, fr.camera_indoors, fr.exterior_visible, fr.water_visible,
			fr.bldg_batched, fr.bldg_visible, fr.bldg_mask, fr.window_groups, fr.gates.terrain,
			fr.gates.sky, fr.visible_buildings, fr.culled.size());
}

std::string set_diff(const std::set<int> &a, const std::set<int> &b) {
	std::string s;
	for (int id : a)
		if (b.count(id) == 0) s += " " + std::to_string(id);
	return s;
}

template <typename F>
int transitions(const std::vector<Frame> &frames, F signal) {
	int n = 0;
	for (size_t i = 1; i < frames.size(); ++i)
		if (signal(frames[i]) != signal(frames[i - 1])) ++n;
	return n;
}

struct LegFlips {
	int letters = 0, terrain = 0, sky = 0, camera_inside = 0, bldg_visible = 0;
	int max_entity = 0, max_building = 0;
};

// The transition rows of one leg (every row with --verbose) and its flip
// counts: over a monotonic walk every gate and verdict flips at most once.
LegFlips report_leg(const std::vector<Frame> &frames, bool verbose) {
	for (size_t i = 0; i < frames.size(); ++i) {
		const Frame &fr = frames[i];
		const Frame *pv = i > 0 ? &frames[i - 1] : nullptr;
		const bool changed = pv == nullptr || fr.letters != pv->letters ||
				fr.camera_hit0 != pv->camera_hit0 || fr.player_hit0 != pv->player_hit0 ||
				fr.camera_indoors != pv->camera_indoors ||
				fr.exterior_visible != pv->exterior_visible || fr.bldg_mask != pv->bldg_mask ||
				fr.bldg_visible != pv->bldg_visible || fr.gates.terrain != pv->gates.terrain ||
				fr.visible_buildings != pv->visible_buildings || fr.culled != pv->culled;
		if (verbose || changed) print_frame(fr);
		if (!fr.watch.empty() && (verbose || changed || pv == nullptr || fr.watch != pv->watch))
			std::printf("        watch%s\n", fr.watch.c_str());
		if (g_occluders && (verbose || changed || pv == nullptr || fr.occluders != pv->occluders))
			std::printf("        occl%s\n", fr.occluders.c_str());
		if (g_slots && (verbose || changed || pv == nullptr || fr.slots != pv->slots))
			std::printf("        slots%s\n", fr.slots.c_str());
		if (pv != nullptr && fr.culled != pv->culled)
			std::printf("        culled +[%s ] -[%s ]\n", set_diff(fr.culled, pv->culled).c_str(),
					set_diff(pv->culled, fr.culled).c_str());
		if (pv != nullptr && fr.hidden_buildings != pv->hidden_buildings)
			std::printf("        bldg hidden +[%s ] -[%s ]\n",
					set_diff(fr.hidden_buildings, pv->hidden_buildings).c_str(),
					set_diff(pv->hidden_buildings, fr.hidden_buildings).c_str());
	}
	LegFlips f;
	f.letters = transitions(frames, [](const Frame &x) { return x.letters; });
	f.terrain = transitions(frames, [](const Frame &x) { return x.gates.terrain; });
	f.sky = transitions(frames, [](const Frame &x) { return x.gates.sky; });
	f.camera_inside = transitions(frames, [](const Frame &x) { return x.camera_indoors; });
	f.bldg_visible = transitions(frames, [](const Frame &x) { return x.bldg_visible; });
	std::map<int, int> entity_flips, building_flips;
	for (size_t i = 1; i < frames.size(); ++i) {
		std::set<int> all = frames[i].culled;
		all.insert(frames[i - 1].culled.begin(), frames[i - 1].culled.end());
		for (int id : all)
			if (frames[i].culled.count(id) != frames[i - 1].culled.count(id)) ++entity_flips[id];
		std::set<int> ball = frames[i].hidden_buildings;
		ball.insert(frames[i - 1].hidden_buildings.begin(), frames[i - 1].hidden_buildings.end());
		for (int id : ball)
			if (frames[i].hidden_buildings.count(id) != frames[i - 1].hidden_buildings.count(id))
				++building_flips[id];
	}
	for (const auto &kv : entity_flips) {
		f.max_entity = std::max(f.max_entity, kv.second);
		if (kv.second > 1) std::printf("leg: entity bms=%d flipped %d times\n", kv.first, kv.second);
	}
	for (const auto &kv : building_flips) {
		f.max_building = std::max(f.max_building, kv.second);
		if (kv.second > 1) std::printf("leg: building bms=%d flipped %d times\n", kv.first, kv.second);
	}
	std::printf("leg: %zu frames; flips letters %d terrain %d sky %d camera_inside %d bldg_visible %d max_entity %d max_building %d\n",
			frames.size(), f.letters, f.terrain, f.sky, f.camera_inside, f.bldg_visible,
			f.max_entity, f.max_building);
	return f;
}

// Walk one leg holding forward until `done` says stop (or max ticks).
template <typename Done>
std::vector<Frame> walk(testrig::RetailMissionRig &rig, const w::Entity &bldg,
		const DoorRecord &door, const float inward[3], int max_ticks, Done done) {
	std::vector<Frame> frames;
	rig.local.input.forward = true;
	for (int i = 0; i < max_ticks; ++i) {
		rig.tick();
		Frame fr;
		if (!run_frame(rig, bldg, door, inward, fr)) continue;
		frames.push_back(fr);
		if (done(fr)) break;
	}
	rig.local.input.forward = false;
	return frames;
}

} // namespace

int main(int argc, char **argv) {
	Options opt;
	for (int i = 1; i < argc; ++i) {
		const std::string a = argv[i];
		if (a == "--bms" && i + 1 < argc) opt.bms = argv[++i];
		else if (a == "--list") opt.list = true;
		else if (a == "--scan") opt.scan = true;
		else if (a == "--spawn") opt.spawn = true;
		else if (a == "--why") opt.why = true;
		else if (a == "--eye") opt.eye = true;
		else if (a == "--retail-truth") opt.retail_truth = true;
		else if (a == "--sweep-out" && i + 1 < argc) opt.sweep_step = static_cast<float>(std::atof(argv[++i]));
		else if (a == "--idle" && i + 1 < argc) opt.idle = std::atoi(argv[++i]);
		else if (a == "--radius" && i + 1 < argc) opt.radius = static_cast<float>(std::atof(argv[++i]));
		else if (a == "--yaw-step" && i + 1 < argc) opt.yaw_step = std::max(1, std::atoi(argv[++i]));
		else if (a == "--bms-id" && i + 1 < argc) opt.bms_id = std::atoi(argv[++i]);
		else if (a == "--door" && i + 1 < argc) opt.door = std::atoi(argv[++i]);
		else if (a == "--tp") opt.third_person = true;
		else if (a == "--watch" && i + 1 < argc) g_watch.push_back(std::atoi(argv[++i]));
		else if (a == "--occluders") g_occluders = true;
		else if (a == "--slots") g_slots = true;
		else if (a == "--verbose") opt.verbose = true;
		else if (a == "--span" && i + 1 < argc) opt.span = static_cast<float>(std::atof(argv[++i]));
		else if (a == "--inside" && i + 1 < argc) opt.inside = static_cast<float>(std::atof(argv[++i]));
		else if (a == "--route" && i + 1 < argc) {
			std::string r = argv[++i];
			size_t at = 0;
			while (at < r.size()) {
				const size_t semi = r.find(';', at);
				const std::string pt = r.substr(at, semi == std::string::npos ? std::string::npos : semi - at);
				const size_t comma = pt.find(',');
				if (comma != std::string::npos)
					opt.route.emplace_back(static_cast<float>(std::atof(pt.substr(0, comma).c_str())),
							static_cast<float>(std::atof(pt.substr(comma + 1).c_str())));
				if (semi == std::string::npos) break;
				at = semi + 1;
			}
		}
	}
	RETAIL_REQUIRE_OR_SKIP(install, retail::install(),
			"OPENNOVA_JO_DIR (a retail JO install carrying the mission)");
	testrig::RetailMissionRig rig;
	std::string error;
	if (!rig.open(install, opt.bms.c_str(), error)) return retail::skip(error.c_str());
	testrig::BootOptions options;
	if (!expect(rig.boot(options, error), "the mission boots")) {
		std::fprintf(stderr, "  %s\n", error.c_str());
		return 1;
	}
	if (!expect(rig.local.has_local_player(), "the host's own player spawned")) return 1;
	rig.tick(62);
	if (opt.list) {
		list_buildings(rig);
		return 0;
	}
	if (opt.spawn) {
		// The authored spawn: where the player stands, the blink boxes and
		// portal records around it, then a look-around sweep of the frame's
		// verdicts from that exact point.
		const w::Vec3 sp = rig.local.player_position();
		const w::Entity *pl = rig.local.player();
		std::printf("spawn: pos (%.2f, %.2f, %.2f) yaw %d letters 0x%02X plhit %s %s\n", sp.x, sp.y, sp.z,
				pl != nullptr ? static_cast<int>(pl->yaw) : 0, rig.collision.local_player_blink_flags,
				hit_str(pl != nullptr ? pl->blink_hits[0] : 0).c_str(),
				hit_str(pl != nullptr ? pl->blink_hits[1] : 0).c_str());
		w::EntityHandle nearest;
		float nearest_d = 1e30f;
		rig.world.registry.for_each([&](const w::Entity &e) {
			if (e.kind != w::EntityKind::Building) return;
			const float d = testrig::planar_distance(e.position, sp);
			if (d > opt.radius) return;
			const bool occ = rig.occlusion.has_instance(e.handle);
			const w::OcclusionWorld::BuildingFlags fl = rig.occlusion.building_flags(e.handle);
			int counts[8] = {};
			for (const DoorRecord &dr : portal_records(rig, e)) counts[std::min(dr.type, 7)]++;
			std::printf("spawn-bldg bms=%d slot=%d %s d=%.1f pos (%.2f, %.2f, %.2f) yaw %d occ=%d recs t0=%d t1=%d t2=%d t3=%d t5=%d win=%d forced=0x%X\n",
					e.bms_id, e.handle.slot(), item_label(rig, e).c_str(), d, e.position.x, e.position.y,
					e.position.z, static_cast<int>(e.yaw), occ, counts[0], counts[1], counts[2], counts[3],
					counts[5], fl.has_windows, rig.occlusion.forced_section_mask(e.handle));
			if (const w::CollisionModel *cm = rig.collision.model_for(rig.world, e.handle)) {
				for (size_t si = 0; si < cm->sections.size(); ++si) {
					const w::CollisionSection &sec = cm->sections[si];
					for (int32_t vi = 0; vi < sec.volume_count; ++vi) {
						const w::CollisionVolume &v = cm->volumes[sec.volume_start + vi];
						if (v.type != 8) continue;
						std::printf("    BB section %zu flags 0x%02X accum 0x%02X local x %.2f..%.2f y %.2f..%.2f z %.2f..%.2f\n",
								si, v.flags, v.flags ^ 6u, v.min_x / 65536.0, v.max_x / 65536.0,
								v.min_y / 65536.0, v.max_y / 65536.0, v.min_z / 65536.0, v.max_z / 65536.0);
					}
				}
			}
			for (const DoorRecord &dr : portal_records(rig, e)) {
				if (dr.type == w::kOccRecOccluder) continue;
				std::printf("    rec %d type %d A=%d B=%d at (%.2f, %.2f, %.2f) z %.2f..%.2f n (%.2f, %.2f, %.2f)\n",
						dr.index, dr.type, dr.section_a, dr.section_b, dr.center[0], dr.center[1],
						dr.center[2], dr.min_z, dr.max_z, dr.normal[0], dr.normal[1], dr.normal[2]);
			}
			if (occ && d < nearest_d) {
				nearest_d = d;
				nearest = e.handle;
			}
		});
		rig.world.registry.for_each([&](const w::Entity &e) {
			if ((e.kind == w::EntityKind::Building && w::building_def_row(e)) || e.kind == w::EntityKind::Marker) return;
			if (testrig::planar_distance(e.position, sp) > opt.radius) return;
			std::printf("spawn-ent bms=%d h=%u %s kind %d d=%.1f pos (%.2f, %.2f, %.2f) hits %s %s flags&1=%u\n",
					e.bms_id, static_cast<unsigned>(e.handle.packed), item_label(rig, e).c_str(),
					static_cast<int>(e.kind), testrig::planar_distance(e.position, sp), e.position.x,
					e.position.y, e.position.z, hit_str(e.blink_hits[0]).c_str(),
					hit_str(e.blink_hits[1]).c_str(), static_cast<unsigned>(e.flags & 1u));
		});
		const w::Entity *nb = rig.world.registry.get(nearest);
		if (nb == nullptr) {
			std::printf("spawn: no occlusion building within %.0f u\n", opt.radius);
			return 0;
		}
		DoorRecord none;
		none.center[0] = sp.x;
		none.center[1] = sp.y;
		const float fwd[3] = {0.0f, 1.0f, 0.0f};
		if (opt.idle > 0) {
			// Stand still at the spawn, no input: every tick's verdicts.
			std::vector<Frame> idle;
			for (int i = 0; i < opt.idle; ++i) {
				rig.tick();
				Frame fr;
				if (run_frame(rig, *nb, none, fwd, fr)) idle.push_back(fr);
			}
			std::printf("=== spawn idle %d ticks ===\n", opt.idle);
			const LegFlips f = report_leg(idle, opt.verbose);
			expect(f.max_building <= 1 && f.max_entity <= 1 && f.camera_inside <= 1 && f.letters <= 1,
					"standing still at the spawn keeps every verdict steady");
		}

		if (opt.eye) {
			// The first-person eye the collector reads, recomposed with retail's
			// own chain from the port's inputs: g_view_pos = Position + CameraOffset
			// (+0x6C..+0x74) @0x437fa2..0x437fb7, then (-0x3000, 0, 0) through the
			// fixed Euler matrix of the view triple @0x437fbd..0x438031
			// (Math_BuildFixedPointMatrixFromEulerAngles + TransformPoint22), and
			// copied unchanged into viewport+4 (byte_A78364) before the collect
			// @0x5ca352..0x5ca37b -> Viewport_BuildProjectionMatrix @0x411136.
			const double yaw0 = pl != nullptr ? static_cast<double>(pl->yaw) : 270.0;
			for (const int pitch : {-20, 0, 20}) {
				rig.local.teleport_local_player(sp, yaw0, static_cast<double>(pitch));
				rig.tick(12);
				const w::Entity *pe = rig.local.player();
				const w::AiEntity *pa = rig.local.player_ai();
				const w::LocalPlayerViewFrame vf = rig.local.view_frame();
				if (pe == nullptr || pa == nullptr || !vf.camera_pose_valid) continue;
				const int32_t eye0[3] = {pa->pos[0] + pe->eye_offset_x, pa->pos[1] + pe->eye_offset_y,
						pa->pos[2] + pe->eye_offset_z};
				const int32_t hb = w::bam_heading_from_mission_yaw_deg(static_cast<double>(vf.camera.yaw_deg));
				const int32_t pb = static_cast<int32_t>(std::lround(vf.camera.pitch_deg / w::kDegreesPerBam));
				const int32_t rb = static_cast<int32_t>(std::lround(vf.camera.roll_deg / w::kDegreesPerBam));
				const w::CollisionMatrix m = w::collision_matrix_from_euler(hb, pb, rb, eye0);
				const int32_t back[3] = {-0x3000, 0, 0};
				int32_t retail_eye[3];
				m.transform_point(back, retail_eye);
				int32_t head[3] = {};
				const bool have_head = rig.collision_pose.resolve_skeletal_anchor(
						rig.world, pe->handle, w::SkeletalAnchor::Head, head);
				const float px[3] = {vf.camera.eye[0], vf.camera.eye[1], vf.camera.eye[2]};
				const w::BlinkAccum q = point_query(rig, px);
				std::printf("eye: pitch %+3d | pos (%.4f, %.4f, %.4f) CameraOffset (%.4f, %.4f, %.4f) head-pos (%.4f, %.4f, %.4f)%s\n",
						pitch, pa->pos[0] / 65536.0, pa->pos[1] / 65536.0, pa->pos[2] / 65536.0,
						pe->eye_offset_x / 65536.0, pe->eye_offset_y / 65536.0, pe->eye_offset_z / 65536.0,
						(head[0] - pa->pos[0]) / 65536.0, (head[1] - pa->pos[1]) / 65536.0,
						(head[2] - pa->pos[2]) / 65536.0, have_head ? "" : " (no head anchor)");
				std::printf("eye: pitch %+3d | view yaw %.3f pitch %.3f roll %.3f | port eye (%.4f, %.4f, %.4f) retail-chain eye (%.4f, %.4f, %.4f) | diff %.5f u | camera query %s\n",
						pitch, vf.camera.yaw_deg, vf.camera.pitch_deg, vf.camera.roll_deg, px[0], px[1], px[2],
						retail_eye[0] / 65536.0, retail_eye[1] / 65536.0, retail_eye[2] / 65536.0,
						std::sqrt((px[0] - retail_eye[0] / 65536.0) * (px[0] - retail_eye[0] / 65536.0) +
								(px[1] - retail_eye[1] / 65536.0) * (px[1] - retail_eye[1] / 65536.0) +
								(px[2] - retail_eye[2] / 65536.0) * (px[2] - retail_eye[2] / 65536.0)),
						hit_str(q.hits[0]).c_str());
			}
			rig.local.teleport_local_player(sp, yaw0, 0.0);
			rig.tick(12);
		}
		if (opt.why) {
			// The spawn's facing (the idle/first frame's camera): every
			// non-building entity within range, its blink quad, the
			// collector's blink-hits gate, the frustum sphere, and the final
			// verdict (the render wave's render_TOC runs only on an empty quad).
			rig.local.teleport_local_player(sp, pl != nullptr ? static_cast<double>(pl->yaw) : 0.0, 0.0);
			rig.tick(2);
			Frame fr;
			run_frame(rig, *nb, none, fwd, fr);
			const w::OcclusionFrameCamera cam = g_last_cam;
			rig.world.registry.for_each([&](const w::Entity &e) {
				if ((e.kind == w::EntityKind::Building && w::building_def_row(e)) ||
						e.kind == w::EntityKind::Marker)
					return;
				const float d = testrig::planar_distance(e.position, sp);
				if (d > opt.radius) return;
				int32_t pf[3] = {w::to_fixed(e.position.x), w::to_fixed(e.position.y), w::to_fixed(e.position.z)};
				float pos[3];
				w::render_float_from_fixed(pf, pos);
				bool in_view = true;
				for (int32_t p = 0; p < cam.frustum_count; ++p)
					if (pos[0] * cam.frustum[p][0] + pos[1] * cam.frustum[p][1] + pos[2] * cam.frustum[p][2] +
									cam.frustum[p][3] < -std::max(e.bound_radius, 1.0f))
						in_view = false;
				const bool gate = rig.occlusion.blink_hits_render_active(e.blink_hits);
				std::printf("why: bms=%d h=%u %s d=%.1f hits %s gate=%d view~%d culled=%d\n", e.bms_id,
						static_cast<unsigned>(e.handle.packed), item_label(rig, e).c_str(), d,
						hit_str(e.blink_hits[0]).c_str(), gate, in_view, fr.culled.count(e.bms_id) != 0);
			});
			rig.world.registry.for_each([&](const w::Entity &e) {
				if (e.kind != w::EntityKind::Building || !w::building_def_row(e) || e.bms_id == 0) return;
				const float d = testrig::planar_distance(e.position, sp);
				if (d > opt.radius) return;
				std::printf("why-bldg: bms=%d %s d=%.1f batched=%d visible=%d mask=0x%08X\n", e.bms_id,
						item_label(rig, e).c_str(), d, rig.occlusion.building_batched(e.handle),
						rig.occlusion.building_visible(e.handle), rig.occlusion.section_mask(e.handle));
			});
		}
		if (opt.retail_truth) {
			// What retail draws at the Ghost Harvest spawn, read from onhook
			// captures of the retail host (the player deployed at its authored
			// spawn (1161.5025, 436.5958, 21.8551), matching this rig's to 1 mm):
			//  - facing west through HN_Bld1's west door (the type-2 record on
			//    x = 1161.30), with the eye 0.06..0.18 u INSIDE that plane at the
			//    spawn and two doorway steps: the hangar Mhangar2 (bms 297) and
			//    the buggy Dbuggy1 (bms 2539) outside draw, as at pitch -20/+20;
			//  - facing north from the spawn: the armory crate Armry03 (bms 2538,
			//    an items.def DECORATION inside the room, section 5), the hangar
			//    and the light post Lpost01 (bms 672) draw.
			struct RetailPose {
				const char *capture;
				float body_x;
				double yaw, pitch;
				std::vector<int> drawn;
			};
			const std::vector<RetailPose> poses = {
					{"r_spawn_1 (the deployed spawn)", 1161.5025f, 270.0, 0.0, {297, 2539}},
					{"r_walk_1161.60", 1161.60f, 270.0, 0.0, {297, 2539}},
					{"r_walk_1161.45", 1161.45f, 270.0, 0.0, {297, 2539}},
					{"r_w_m20", 1161.5025f, 270.0, -20.0, {297, 2539}},
					{"r_w_p20", 1161.5025f, 270.0, 20.0, {297}},
					{"r_n_0", 1161.5025f, 0.0, 0.0, {297, 672, 2538}},
			};
			if (!expect(opt.bms == "CP03.bms", "--retail-truth carries the CP03 captures")) return 1;
			for (const RetailPose &pose : poses) {
				rig.local.teleport_local_player(w::Vec3{pose.body_x, 436.5958f, 21.8551f}, pose.yaw,
						pose.pitch);
				rig.tick(2);
				Frame fr;
				if (!expect(run_frame(rig, *nb, none, fwd, fr), "the retail pose has a camera")) continue;
				std::printf("retail-truth: %s body x %.4f eye (%.4f, %.4f, %.4f) yaw %.0f pitch %.0f in=%d ext=%d wg=%d mask 0x%08X vis_bldg=%d\n",
						pose.capture, fr.pos[0], fr.eye[0], fr.eye[1], fr.eye[2], pose.yaw, pose.pitch,
						fr.camera_indoors, fr.exterior_visible, fr.window_groups, fr.bldg_mask,
						fr.visible_buildings);
				for (const int id : pose.drawn) {
					bool present = false;
					rig.world.registry.for_each([&](const w::Entity &e) {
						if (e.bms_id == id) present = true;
					});
					const bool drawn = present && fr.culled.count(id) == 0 && fr.hidden_buildings.count(id) == 0;
					std::printf("retail-truth:   bms %d %s\n", id, drawn ? "drawn" : "NOT DRAWN");
					char msg[160];
					std::snprintf(msg, sizeof(msg), "%s: bms %d draws as in retail", pose.capture, id);
					expect(drawn, msg);
				}
			}
			rig.local.teleport_local_player(sp, pl != nullptr ? static_cast<double>(pl->yaw) : 0.0, 0.0);
			rig.tick(2);
		}
		std::vector<Frame> frames;
		for (int pitch = -20; pitch <= 20; pitch += 20) {
			for (int yaw = 0; yaw < 360; yaw += opt.yaw_step) {
				rig.local.teleport_local_player(sp, static_cast<double>(yaw), static_cast<double>(pitch));
				rig.tick(2);
				Frame fr;
				if (run_frame(rig, *nb, none, fwd, fr)) frames.push_back(fr);
			}
		}
		std::printf("=== spawn look-around (nearest occlusion building bms=%d) ===\n", nb->bms_id);
		report_leg(frames, true);
		if (failures == 0) std::printf("indoor_visibility_walk: spawn checks passed\n");
		return failures == 0 ? 0 : 1;
	}
	if (opt.scan) {
		// Every non-building entity: its blink quad, and for one with an empty
		// quad whether it sits inside an occlusion building's blink boxes by
		// the point query (a stale/missing quad) — the entities render_TOC's
		// camera-inside early rule culls from inside that building.
		int contained = 0, stale = 0;
		rig.world.registry.for_each([&](const w::Entity &e) {
			if ((e.kind == w::EntityKind::Building && w::building_def_row(e)) || e.kind == w::EntityKind::Marker) return;
			const float p[3] = {e.position.x, e.position.y, e.position.z};
			const w::BlinkAccum q = point_query(rig, p);
			if (e.blink_hits[0] != 0) ++contained;
			if (e.blink_hits[0] == 0 && q.hits[0] == 0) return;
			if (e.blink_hits[0] != 0 && q.hits[0] == e.blink_hits[0] && !opt.verbose) return;
			if (e.blink_hits[0] == 0) ++stale;
			std::printf("scan: bms=%d h=%u pool=%d %s kind %d pos (%.2f, %.2f, %.2f) hits %s query %s%s\n",
					e.bms_id, static_cast<unsigned>(e.handle.packed), e.handle.pool(),
					item_label(rig, e).c_str(), static_cast<int>(e.kind), e.position.x, e.position.y,
					e.position.z, hit_str(e.blink_hits[0]).c_str(), hit_str(q.hits[0]).c_str(),
					e.blink_hits[0] == 0 ? "  <- EMPTY QUAD INSIDE A BOX" : "");
		});
		std::printf("scan: %d entities carry a blink quad; %d sit in a box with an empty quad\n",
				contained, stale);
		return 0;
	}
	if (opt.third_person) {
		rig.local.view.debug_third_person_on_foot = true;
		w::player_view_resolve_mode(rig.local.view);
	}

	// The building and its door: a type-2 (exterior) record tall enough to
	// walk through whose bottom sits near the floor, nearest the spawn.
	const w::Vec3 spawn = rig.local.player_position();
	w::EntityHandle bldg_handle;
	DoorRecord door;
	float best = 1e30f;
	rig.world.registry.for_each([&](const w::Entity &e) {
		if (e.kind != w::EntityKind::Building || !rig.occlusion.has_instance(e.handle)) return;
		if (opt.bms_id != 0 && e.bms_id != opt.bms_id) return;
		for (const DoorRecord &d : portal_records(rig, e)) {
			if (d.type != w::kOccRecWindow) continue;
			if (opt.door >= 0 && d.index != opt.door) continue;
			const float ground = rig.ground_height(d.center[0], d.center[1]);
			const bool floor_level = std::fabs(d.min_z - ground) < 0.8f ||
					std::fabs(d.min_z - e.position.z) < 0.8f;
			if (opt.door < 0 && (d.max_z - d.min_z < 1.7f || !floor_level)) continue;
			const float dist = testrig::planar_distance(e.position, spawn);
			if (dist < best) {
				best = dist;
				bldg_handle = e.handle;
				door = d;
			}
		}
	});
	const w::Entity *bldg = rig.world.registry.get(bldg_handle);
	if (!expect(bldg != nullptr, "a building with a floor-level exterior (type-2) record")) return 1;
	std::printf("walk: building bms=%d slot=%d %s pos (%.2f, %.2f, %.2f) yaw %d; door rec %d A=%d B=%d at (%.2f, %.2f, %.2f) z %.2f..%.2f n (%.2f, %.2f, %.2f)\n",
			bldg->bms_id, bldg->handle.slot(), item_label(rig, *bldg).c_str(), bldg->position.x,
			bldg->position.y, bldg->position.z, static_cast<int>(bldg->yaw), door.index,
			door.section_a, door.section_b, door.center[0], door.center[1], door.center[2],
			door.min_z, door.max_z, door.normal[0], door.normal[1], door.normal[2]);

	// Inward = the side whose blink query lands in this building.
	float nxy[2] = {door.normal[0], door.normal[1]};
	const float nlen = std::sqrt(nxy[0] * nxy[0] + nxy[1] * nxy[1]);
	if (!expect(nlen > 0.1f, "the door record's plane is not horizontal")) return 1;
	nxy[0] /= nlen;
	nxy[1] /= nlen;
	float inward[3] = {nxy[0], nxy[1], 0.0f};
	{
		const float probe_z = door.min_z + 1.0f;
		const float plus[3] = {door.center[0] + nxy[0] * 1.5f, door.center[1] + nxy[1] * 1.5f, probe_z};
		const float minus[3] = {door.center[0] - nxy[0] * 1.5f, door.center[1] - nxy[1] * 1.5f, probe_z};
		const w::BlinkAccum ap = point_query(rig, plus);
		const w::BlinkAccum am = point_query(rig, minus);
		const bool plus_in = ap.hits[0] != 0 &&
				w::BlinkAccum::hit_pool_entity_index(ap.hits[0]) == bldg->handle.slot();
		const bool minus_in = am.hits[0] != 0 &&
				w::BlinkAccum::hit_pool_entity_index(am.hits[0]) == bldg->handle.slot();
		std::printf("walk: +n side hit %s flags 0x%02X, -n side hit %s flags 0x%02X\n",
				hit_str(ap.hits[0]).c_str(), ap.flags, hit_str(am.hits[0]).c_str(), am.flags);
		if (minus_in && !plus_in) {
			inward[0] = -nxy[0];
			inward[1] = -nxy[1];
		}
		expect(plus_in != minus_in, "exactly one side of the door lies in the building's blink boxes");
	}

	// The entities around the door: label, position, blink hits.
	{
		const w::Vec3 dc{door.center[0], door.center[1], door.center[2]};
		rig.world.registry.for_each([&](const w::Entity &e) {
			if ((e.kind == w::EntityKind::Building && w::building_def_row(e)) || e.kind == w::EntityKind::Marker) return;
			if (testrig::planar_distance(e.position, dc) > 25.0f) return;
			std::printf("near: bms=%d %s kind %d pos (%.2f, %.2f, %.2f) hits %s %s %s flags&1=%u\n",
					e.bms_id, item_label(rig, e).c_str(), static_cast<int>(e.kind), e.position.x,
					e.position.y, e.position.z, hit_str(e.blink_hits[0]).c_str(),
					hit_str(e.blink_hits[1]).c_str(), hit_str(e.blink_hits[2]).c_str(),
					static_cast<unsigned>(e.flags & 1u));
		});
	}

	if (opt.sweep_step > 0.0f) {
		// Settled static poses out through the doorway, facing out. After a
		// few dozen collects every latch has re-probed, so each verdict is a
		// function of the pose alone: the poses a retail capture reproduces.
		const double yaw_out = std::atan2(-inward[0], -inward[1]) * 180.0 / kPi;
		std::vector<Frame> frames;
		for (float along = 1.2f; along >= -6.0f; along -= opt.sweep_step) {
			const float x = door.center[0] + inward[0] * along;
			const float y = door.center[1] + inward[1] * along;
			const float z = std::max(rig.ground_height(x, y), door.min_z) + 0.05f;
			rig.local.teleport_local_player(w::Vec3{x, y, z}, yaw_out, 0.0);
			rig.tick(40);
			Frame fr;
			bool ok = false;
			for (int settle = 0; settle < 30; ++settle) ok = run_frame(rig, *bldg, door, inward, fr);
			if (ok) frames.push_back(fr);
		}
		std::printf("=== sweep out (settled static poses, facing out) ===\n");
		report_leg(frames, true);
		return failures == 0 ? 0 : 1;
	}

	// Leg in: start span u outside facing the door, then hold forward.
	const float sx = door.center[0] - inward[0] * opt.span;
	const float sy = door.center[1] - inward[1] * opt.span;
	const float start_z = std::max(rig.ground_height(sx, sy), door.min_z) + 0.05f;
	const double yaw_in = std::atan2(inward[0], inward[1]) * 180.0 / kPi;
	rig.local.teleport_local_player(w::Vec3{sx, sy, start_z}, yaw_in, 0.0);
	rig.local.input.forward = false;
	rig.tick(40);
	const std::vector<Frame> in_frames = walk(rig, *bldg, door, inward, 900,
			[&](const Frame &fr) { return fr.along > opt.inside; });
	std::printf("=== leg in (facing in) ===\n");
	const LegFlips in_flips = report_leg(in_frames, opt.verbose);
	if (!expect(!in_frames.empty() && in_frames.back().along > 1.0f, "the walk reached the interior"))
		return 1;

	// Leg out: face the door from inside and walk back out, the camera
	// looking through the doorway as it crosses it.
	rig.local.teleport_local_player(rig.local.player_position(), yaw_in + 180.0, 0.0);
	rig.tick(20);
	const std::vector<Frame> out_frames = walk(rig, *bldg, door, inward, 900,
			[&](const Frame &fr) { return fr.along < -opt.span; });
	std::printf("=== leg out (facing out) ===\n");
	const LegFlips out_flips = report_leg(out_frames, opt.verbose);
	expect(!out_frames.empty() && out_frames.back().along < -1.0f, "the walk back out left the building");

	// Optional route: walk the waypoints (re-entering through the door first).
	if (!opt.route.empty()) {
		rig.local.teleport_local_player(w::Vec3{sx, sy, start_z}, yaw_in, 0.0);
		rig.tick(40);
		std::vector<Frame> route_frames;
		for (const auto &wp : opt.route) {
			for (int i = 0; i < 600; ++i) {
				const w::Vec3 p = rig.local.player_position();
				const float dx = wp.first - p.x, dy = wp.second - p.y;
				if (std::sqrt(dx * dx + dy * dy) < 0.35f) break;
				const double yaw = std::atan2(dx, dy) * 180.0 / kPi;
				rig.local.teleport_local_player(p, yaw, 0.0);
				rig.local.input.forward = true;
				rig.tick();
				Frame fr;
				if (run_frame(rig, *bldg, door, inward, fr)) route_frames.push_back(fr);
			}
			rig.local.input.forward = false;
		}
		std::printf("=== leg route ===\n");
		report_leg(route_frames, opt.verbose);
	}

	// The per-crossing laws that hold in retail on a monotonic walk: the
	// letters come from the body's pass-1 capsule points and the camera query
	// from the eye, each crossing the box boundary once, and the pass gates
	// follow the letters. [orig: Entity_MovementCollisionResolver — the
	// letter OR inside the first-pass candidate loop @ 0x4b34c0..0x4b3502;
	// Terrain_BuildSectorVisibilityMasks — the camera query at the render eye
	// byte_A78364 @ 0x5c8674..0x5c8679]
	expect(in_flips.letters <= 1 && out_flips.letters <= 1,
			"(B) the local letters flip at most once per doorway crossing");
	expect(in_flips.terrain <= 1 && out_flips.terrain <= 1,
			"(B) the terrain gate flips at most once per doorway crossing");
	expect(in_flips.sky <= 1 && out_flips.sky <= 1, "(B) the sky gate flips at most once per doorway crossing");
	expect(in_flips.camera_inside <= 1 && out_flips.camera_inside <= 1,
			"(B) the camera-inside verdict flips at most once per doorway crossing");
	// Diagnostic only: repeated building/entity flips across a crossing are
	// retail law here — the 14-slot clamp over collection order (the zero
	// OOBJ weights) and the outdoors-only bit-31 marker that arms every type-1
	// hull when the camera leaves the box [orig: Terrain_SortPortalSlotsByPriority
	// @ 0x5c449f..0x5c44a4; Terrain_BuildSectorVisibilityMasks @ 0x5c86e7..0x5c871b;
	// Terrain_TestSectorEntityOcclusion's type-1 gate @ 0x5c4764].
	std::printf("walk: diagnostic max flips building %d/%d entity %d/%d (in/out)\n",
			in_flips.max_building, out_flips.max_building, in_flips.max_entity, out_flips.max_entity);

	if (failures == 0) std::printf("indoor_visibility_walk: all checks passed\n");
	return failures == 0 ? 0 : 1;
}
