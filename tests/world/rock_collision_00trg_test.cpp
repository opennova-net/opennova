// The rock directly in front of the sole 00TRg single-player start, on the
// retail data: the production mission promotion -> .3di collision IR ->
// RoundSim path, with no rendering. RckS05 (bms 650, item 101471) must expose
// its 52 authored CFAC faces, its collision vertices must sit on the render
// mesh under the same placement matrix (no collision/render drift), and rays
// through real LOD-0 render-triangle centroids at the four angular silhouette
// extremes — directions guaranteed to cross authored visible geometry — must
// strike the entity's CFAC mesh (a miss there is a concrete render-vs-bullet
// hole, not an AABB approximation artifact).
// Gated on OPENNOVA_JO_ASSETS (an extracted retail tree carrying 00TRg.bms).
#include "common/retail_mission_rig.h"
#include "common/retail_paths.h"

#include <formats/threedi/threedi_strip_decode.h>
#include <runtime/world/angle.h>
#include <runtime/world/round_sim.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

namespace {

using namespace opennova;
namespace w = opennova::world;

int failures = 0;
bool expect(bool cond, const char *msg) {
	if (cond) return true;
	std::fprintf(stderr, "FAIL: %s\n", msg);
	++failures;
	return false;
}

constexpr int kSpawnBmsId = 1197;
constexpr int kTargetBmsId = 650;
constexpr int kTargetItemId = 101471;
constexpr const char *kTargetGraphic = "RckS05";
constexpr int kTargetFaceCount = 52;
constexpr float kMaxVertexError = 0.02f;
constexpr const char *kAmmo = "AMMO_M16_556MM";
constexpr float kEyeHeight = 1.6f;

using V = w::Vec3;
V sub(const V &a, const V &b) { return V{a.x - b.x, a.y - b.y, a.z - b.z}; }
V add(const V &a, const V &b) { return V{a.x + b.x, a.y + b.y, a.z + b.z}; }
V scale(const V &a, float s) { return V{a.x * s, a.y * s, a.z * s}; }
float dot(const V &a, const V &b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
V cross(const V &a, const V &b) { return V{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
float length(const V &a) { return std::sqrt(dot(a, a)); }
V normalized(const V &a) {
	const float l = length(a);
	return l > 0.0f ? scale(a, 1.0f / l) : a;
}

std::string lower(std::string s) {
	for (char &c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	return s;
}

const bms::Entity *find_record(const bms::File &mission, int id) {
	for (const std::vector<bms::Entity> *group : {&mission.items, &mission.buildings, &mission.markers, &mission.organics})
		for (const bms::Entity &e : *group)
			if (e.id == id) return &e;
	return nullptr;
}

// The LOD-0 render triangles of a model under a placement matrix (mission
// space): the strips de-stripped, the render vertices carried into the
// collision workspace frame and transformed through the same fixed-point
// matrix the collision shell uses. The 3DI reader stores RDTA render
// vertices already in engine space while CVRT collision geometry stays in
// workspace space; RDTA reaches engine space via (-y, z, x), so a render
// vertex (x, y, z) is the workspace point (z, -x, y)
// (godot/src/object/object_data_geometry.cpp, the validated frame note).
struct RenderMesh {
	std::vector<V> points;
	std::vector<std::array<V, 3>> triangles;
};

V transform(const w::CollisionMatrix &m, const V &p) {
	const int32_t in[3] = {static_cast<int32_t>(p.x * 65536.0f), static_cast<int32_t>(p.y * 65536.0f),
			static_cast<int32_t>(p.z * 65536.0f)};
	int32_t out[3];
	m.transform_point(in, out);
	return V{out[0] / 65536.0f, out[1] / 65536.0f, out[2] / 65536.0f};
}

V render_to_workspace(const ThreediVertex &v) {
	return V{v.position[2], -v.position[0], v.position[1]};
}

RenderMesh render_mesh(const Threedi3di3 &model, const w::CollisionMatrix &placement) {
	RenderMesh mesh;
	if (model.lod_count == 0 || model.lods == nullptr) return mesh;
	const ThreediLod &lod = model.lods[0];
	std::vector<uint16_t> indices;
	for (size_t s = 0; s < lod.strip_count; ++s) {
		const ThreediTriangleStrip &strip = lod.strips[s];
		if (!threedi_decode_strip_indices(lod, strip, indices)) continue;
		const auto vertex = [&](uint16_t local) {
			const ThreediVertex &v = lod.vertices.items[static_cast<uint32_t>(strip.start_vertex) + local];
			return transform(placement, render_to_workspace(v));
		};
		for (size_t i = 0; i + 2 < indices.size(); i += 3) {
			const std::array<V, 3> tri{vertex(indices[i]), vertex(indices[i + 1]), vertex(indices[i + 2])};
			mesh.triangles.push_back(tri);
			for (const V &p : tri) mesh.points.push_back(p);
		}
	}
	return mesh;
}

bool ray_hits_triangles(const V &origin, const V &dir, const std::vector<V> &tris) {
	for (size_t base = 0; base + 2 < tris.size(); base += 3) {
		const V e1 = sub(tris[base + 1], tris[base]);
		const V e2 = sub(tris[base + 2], tris[base]);
		const V h = cross(dir, e2);
		const float det = dot(e1, h);
		if (std::fabs(det) < 0.000001f) continue;
		const float inv = 1.0f / det;
		const V s = sub(origin, tris[base]);
		const float u = dot(s, h) * inv;
		if (u < 0.0f || u > 1.0f) continue;
		const V q = cross(s, e1);
		const float v = dot(dir, q) * inv;
		if (v < 0.0f || u + v > 1.0f) continue;
		if (dot(e2, q) * inv > 0.0001f) return true;
	}
	return false;
}

struct Fire {
	bool hit = false;
	int face_misses = 0;
	std::string terminal = "no_event";
};

Fire fire_one(testrig::RetailMissionRig &rig, const V &origin, const V &dir, uint16_t target, int ammo_index) {
	static const char *const kKindNames[] = {"organic", "item face", "item sphere", "terrain", "expired", "face miss"};
	const uint32_t spawn_tick = rig.world.logic_tick;
	w::RoundSpawnParams params;
	params.owner = rig.world.cached.local_player;
	params.shooter_handle = params.owner.valid() ? params.owner.packed : 0xFFFF;
	params.ammo_index = ammo_index;
	params.origin = origin;
	params.dir_yaw_bam = testrig::bam_from_radians(std::atan2(dir.y, dir.x));
	params.dir_pitch_bam = testrig::bam_from_radians(std::asin(std::max(-1.0f, std::min(1.0f, dir.z))));
	Fire out;
	const int slot = rig.world.round_sim.spawn(rig.world, params);
	if (slot < 0) {
		out.terminal = "spawn_failed";
		return out;
	}
	// A 5.56 round covers ~15 u per tick: the rock 7 u ahead stops it within
	// its first tick, stamped with the spawn tick itself.
	rig.tick(384);
	const w::RoundSim &rs = rig.world.round_sim;
	const int count = rs.debug_trail_count;
	int idx = (rs.debug_trail_next - count + w::RoundSim::kDebugTrailCap * 2) % w::RoundSim::kDebugTrailCap;
	for (int i = 0; i < count; ++i, idx = (idx + 1) % w::RoundSim::kDebugTrailCap) {
		const w::RoundDebugEvent &ev = rs.debug_trail[idx];
		if (ev.tick < spawn_tick) continue;
		if (ev.entity == target && ev.kind == w::RoundDebugEvent::kItemFace) {
			out.hit = true;
			out.terminal = "item_face";
		} else if (ev.entity == target && ev.kind == w::RoundDebugEvent::kFaceMiss) {
			++out.face_misses;
		}
		if (ev.kind != w::RoundDebugEvent::kFaceMiss && ev.kind < 6) out.terminal = kKindNames[ev.kind];
	}
	return out;
}

} // namespace

int main() {
	RETAIL_REQUIRE_OR_SKIP(assets, retail::assets(),
			"OPENNOVA_JO_ASSETS (an extracted retail tree carrying 00TRg.bms)");
	testrig::RetailMissionRig rig;
	std::string error;
	if (!rig.open(assets, "00TRg.bms", error)) return retail::skip(error.c_str());
	if (!expect(rig.items_ok, "items.def parses from the tree")) return 1;
	const bms::Entity *spawn_record = find_record(rig.mission, kSpawnBmsId);
	const bms::Entity *target_record = find_record(rig.mission, kTargetBmsId);
	if (!expect(spawn_record != nullptr && target_record != nullptr, "the fixture records (spawn 1197, target 650) exist"))
		return 1;
	const int target_item = static_cast<int>(target_record->type_id) + static_cast<int>(mission::kItemIdOffset);
	const DefItemDef *target_def = simassets::find_item_def(rig.items, target_item);
	if (!expect(target_item == kTargetItemId && target_def != nullptr && lower(target_def->graphic) == lower(kTargetGraphic),
				"entity 650 keeps its identity (item 101471, graphic RckS05)"))
		return 1;

	// The bare production promotion -> collision IR -> RoundSim path: no
	// terrain (the rays cross the rock's own geometry), no scripts, no host
	// session — the rounds are spawned straight into the round sim.
	testrig::BootOptions options;
	options.terrain = false;
	options.wac = false;
	options.listen_server = false;
	if (!expect(rig.boot(options, error), "00TRg promotes with collision")) {
		std::fprintf(stderr, "  %s\n", error.c_str());
		return 1;
	}
	if (!expect(rig.has_local_player(), "the local player spawned at the start")) return 1;
	if (!expect(rig.collision_attached > 0, "collision instances attached")) return 1;
	const int ammo_index = rig.world.ammo.index_of(kAmmo);
	if (!expect(ammo_index >= 0, "ammo.def carries AMMO_M16_556MM")) return 1;

	const w::AiEntity *pai = rig.player_ai();
	const V player_pos = rig.player_position();
	const double yaw_deg = w::mission_yaw_deg_from_bam_heading(pai->heading);
	const V eye{player_pos.x, player_pos.y, player_pos.z + kEyeHeight};
	const double yaw_rad = yaw_deg * 3.14159265358979323846 / 180.0;
	const V spawn_forward = normalized(V{float(std::sin(yaw_rad)), float(std::cos(yaw_rad)), 0.0f});

	w::Entity *target = rig.by_bms_id(kTargetBmsId);
	if (!expect(target != nullptr, "entity 650 promoted")) return 1;
	const w::CollisionMatrix placement = w::entity_placement_matrix(*target);
	const Threedi3di3 *model = rig.models.model_for(kTargetGraphic);
	if (!expect(model != nullptr, "RckS05.3di parses through the sim's model cache")) return 1;
	const RenderMesh render = render_mesh(*model, placement);
	if (!expect(!render.triangles.empty(), "RckS05 has LOD-0 render triangles")) return 1;
	V vmin = render.points[0], vmax = render.points[0];
	for (const V &p : render.points) {
		vmin = V{std::min(vmin.x, p.x), std::min(vmin.y, p.y), std::min(vmin.z, p.z)};
		vmax = V{std::max(vmax.x, p.x), std::max(vmax.y, p.y), std::max(vmax.z, p.z)};
	}
	const V center = scale(add(vmin, vmax), 0.5f);
	const V size = sub(vmax, vmin);

	// The projectile hitbox entry of the target: the nearest debug entity.
	const std::vector<w::CollisionWorld::DebugHitboxEntity> hitboxes = rig.hitboxes(target->position, 80.0f, 96, 24000);
	const w::CollisionWorld::DebugHitboxEntity *target_box = nullptr;
	float best = 0.05f;
	for (const w::CollisionWorld::DebugHitboxEntity &e : hitboxes) {
		const V pos{e.pos[0] / 65536.0f, e.pos[1] / 65536.0f, e.pos[2] / 65536.0f};
		const float d = length(sub(pos, target->position));
		if (d < best) {
			best = d;
			target_box = &e;
		}
	}
	if (!expect(target_box != nullptr, "entity 650 has a projectile hitbox entry")) return 1;
	const uint16_t target_handle = target_box->handle.packed;
	std::vector<V> tris;
	for (const w::CollisionWorld::DebugHitboxFace &f : target_box->faces)
		for (int k = 0; k < 3; ++k) tris.push_back(V{f.v[k][0] / 65536.0f, f.v[k][1] / 65536.0f, f.v[k][2] / 65536.0f});

	// Collision-to-render vertex drift under the shared placement.
	std::vector<V> unique_collision;
	for (const V &p : tris) {
		bool seen = false;
		for (const V &q : unique_collision)
			if (dot(sub(p, q), sub(p, q)) < 0.00000001f) { seen = true; break; }
		if (!seen) unique_collision.push_back(p);
	}
	float worst = 0.0f, total = 0.0f;
	for (const V &p : unique_collision) {
		float nearest = 1e30f;
		for (const V &r : render.points) nearest = std::min(nearest, length(sub(p, r)));
		total += nearest;
		worst = std::max(worst, nearest);
	}
	const int emitted_faces = static_cast<int>(tris.size() / 3);
	std::printf("rock: spawn pos=(%.2f, %.2f, %.2f) yaw=%.2f eye=(%.2f, %.2f, %.2f)\n", player_pos.x, player_pos.y,
			player_pos.z, yaw_deg, eye.x, eye.y, eye.z);
	std::printf("rock: target pos=(%.2f, %.2f, %.2f) handle=%u attached=%d faces=%d/%d visual_aabb=(%.2f,%.2f,%.2f)..(%.2f,%.2f,%.2f)\n",
			target->position.x, target->position.y, target->position.z, unsigned(target_handle), rig.collision_attached,
			emitted_faces, target_box->face_total, vmin.x, vmin.y, vmin.z, vmax.x, vmax.y, vmax.z);
	std::printf("rock: collision_to_render_vertex_error collision=%zu render=%zu mean=%.6f max=%.6f\n",
			unique_collision.size(), render.points.size(),
			unique_collision.empty() ? 0.0f : total / float(unique_collision.size()), worst);
	if (!expect(emitted_faces == kTargetFaceCount && target_box->face_total == kTargetFaceCount,
				"RckS05 exposes its 52 authored collision faces"))
		return 1;
	if (!expect(worst <= kMaxVertexError, "RckS05 collision vertices sit on the render mesh (<= 0.02 u)")) return 1;

	// Rays: the spawn facing, the visual center and four offsets (diagnostic),
	// then the four render-triangle silhouette extremes (required).
	const V right = normalized(V{spawn_forward.y, -spawn_forward.x, 0.0f});
	const float lateral = 0.18f * std::min(size.x, size.y);
	const float vertical = 0.18f * size.z;
	struct Ray {
		std::string name;
		V dir;
		bool required;
	};
	std::vector<Ray> rays{
		{"spawn_forward", spawn_forward, false},
		{"visual_center", normalized(sub(center, eye)), false},
		{"visual_left", normalized(sub(sub(center, scale(right, lateral)), eye)), false},
		{"visual_right", normalized(sub(add(center, scale(right, lateral)), eye)), false},
		{"visual_upper", normalized(sub(V{center.x, center.y, center.z + vertical}, eye)), false},
		{"visual_lower", normalized(sub(V{center.x, center.y, center.z - vertical}, eye)), false},
	};
	std::map<std::string, V> silhouette;
	float left = 1e30f, right_a = -1e30f, lower_a = 1e30f, upper_a = -1e30f;
	for (const std::array<V, 3> &tri : render.triangles) {
		const V centroid = scale(add(add(tri[0], tri[1]), tri[2]), 1.0f / 3.0f);
		const V dir = normalized(sub(centroid, eye));
		const float front = dot(dir, spawn_forward);
		if (front <= 0.0f) continue;
		const float horizontal = std::atan2(dot(dir, right), front);
		const float vert = std::asin(std::max(-1.0f, std::min(1.0f, dir.z)));
		if (horizontal < left) { left = horizontal; silhouette["mesh_left"] = dir; }
		if (horizontal > right_a) { right_a = horizontal; silhouette["mesh_right"] = dir; }
		if (vert < lower_a) { lower_a = vert; silhouette["mesh_lower"] = dir; }
		if (vert > upper_a) { upper_a = vert; silhouette["mesh_upper"] = dir; }
	}
	if (!expect(silhouette.size() == 4, "four render-triangle silhouette rays resolve in front of the eye")) return 1;
	for (const auto &kv : silhouette) rays.push_back({kv.first, kv.second, true});

	std::string misses;
	for (const Ray &ray : rays) {
		const bool debug_cross = ray_hits_triangles(eye, ray.dir, tris);
		const Fire result = fire_one(rig, eye, ray.dir, target_handle, ammo_index);
		std::printf("rock: ray=%-14s required=%d debug_cross=%d hit=%d target_face_misses=%d terminal=%s\n",
				ray.name.c_str(), int(ray.required), int(debug_cross), int(result.hit), result.face_misses,
				result.terminal.c_str());
		if (ray.required && (!debug_cross || !result.hit)) misses += ray.name + " ";
	}
	expect(misses.empty(), ("RckS05 entity 650 missed render-triangle rays: " + misses).c_str());
	if (failures == 0) std::printf("rock_collision_00trg: all 4 render-triangle rays hit entity 650's CFAC mesh\n");
	return failures == 0 ? 0 : 1;
}
