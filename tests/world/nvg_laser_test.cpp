// The NVG IR laser beam (world/nvg_laser.h): the draw gate, the ray clip over
// the static and vehicle pools, and the beam's point run.
// [orig: Entity_RenderNVGLaserBeam @ 0x5c6090]
#include <cmath>
#include <cstdio>
#include <memory>

#include <formats/def/def.h>
#include <runtime/world/collision.h>
#include <runtime/world/nvg_laser.h>
#include <runtime/world/world.h>

using namespace opennova::world;

static int failures = 0;

#define CHECK(c)                                                                  \
	do {                                                                          \
		if (!(c)) {                                                               \
			std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c);              \
			++failures;                                                           \
		}                                                                         \
	} while (0)

namespace {

int32_t q16(double v) { return static_cast<int32_t>(v * 65536.0); }

// The gate [orig: @ 0x5c609a..0x5c60e6]: every leg alone blocks the beam.
void test_gate() {
	NvgLaserGate gate;
	gate.has_weapon_def = true;
	gate.weapon_flags = static_cast<int32_t>(opennova::def::DEF_WEAPON_FLAG_LASERBEAM);
	gate.nvg_active = true;
	gate.camera_mode = 0;
	CHECK(nvg_laser_beam_drawn(gate));
	NvgLaserGate mounted = gate;
	mounted.attach_bone = 3;
	CHECK(!nvg_laser_beam_drawn(mounted));
	NvgLaserGate unarmed = gate;
	unarmed.has_weapon_def = false;
	CHECK(!nvg_laser_beam_drawn(unarmed));
	NvgLaserGate plain = gate;
	plain.weapon_flags = 0x8000000; // another flag bit only
	CHECK(!nvg_laser_beam_drawn(plain));
	NvgLaserGate day = gate;
	day.nvg_active = false;
	CHECK(!nvg_laser_beam_drawn(day));
	NvgLaserGate chase = gate;
	chase.camera_mode = 1;
	CHECK(!nvg_laser_beam_drawn(chase));
	NvgLaserGate self = gate;
	self.local_player = true;
	CHECK(!nvg_laser_beam_drawn(self));
}

// The unclipped beam: 32 samples 0.25 u apart from the action point, no clip
// point [orig: @ 0x5c6248..0x5c6298].
void test_points_unclipped() {
	const int32_t origin[3] = {q16(10.0), q16(-4.0), q16(2.5)};
	const int32_t dir[3] = {0, q16(1.0), 0};
	float points[kNvgLaserMaxPoints * 4];
	const int count = nvg_laser_beam_points(origin, dir, kNvgLaserRangeQ16, points);
	CHECK(count == 32);
	CHECK(points[0] == 10.0f && points[1] == -4.0f && points[2] == 2.5f && points[3] == 1.0f);
	CHECK(points[31 * 4 + 1] == -4.0f + 31 * 0.25f);
	CHECK(points[31 * 4 + 0] == 10.0f);
	// A clip beyond the 32nd sample adds no clip point either.
	CHECK(nvg_laser_beam_points(origin, dir, q16(7.9), points) == 32);
}

// A clipped beam: the samples short of the clip, then the clip point twice
// (dir * clip rounded to Q16) [orig: @ 0x5c629d..0x5c637e].
void test_points_clipped() {
	const int32_t origin[3] = {0, 0, q16(1.0)};
	const int32_t dir[3] = {q16(0.6), q16(0.8), 0};
	float points[kNvgLaserMaxPoints * 4];
	const int count = nvg_laser_beam_points(origin, dir, q16(1.1), points);
	// Samples at 0, 0.25, 0.5, 0.75, 1.0 (i << 14 < 1.1), then the clip twice.
	CHECK(count == 7);
	CHECK(std::abs(points[4 * 4 + 0] - 0.6f) < 1.0e-4f);
	CHECK(std::abs(points[4 * 4 + 1] - 0.8f) < 1.0e-4f);
	for (int k = 5; k < 7; ++k) {
		CHECK(std::abs(points[k * 4 + 0] - 0.66f) < 1.0e-4f);
		CHECK(std::abs(points[k * 4 + 1] - 0.88f) < 1.0e-4f);
		CHECK(points[k * 4 + 2] == 1.0f);
		CHECK(points[k * 4 + 3] == 1.0f);
	}
	// A zero clip draws the clip point alone, twice (a two-point run).
	CHECK(nvg_laser_beam_points(origin, dir, 0, points) == 2);
}

// One authored CFAC face in the model's y/z plane (the destruction test's
// projectile wall).
CollisionModel wall_model(double half_extent) {
	CollisionModel model;
	auto vertex = [&](double y, double z) {
		CollisionVertex value;
		value.p[1] = q16(y);
		value.p[2] = q16(z);
		model.vertices.push_back(value);
	};
	vertex(-half_extent, -half_extent);
	vertex(half_extent, -half_extent);
	vertex(0.0, half_extent);
	CollisionNormal normal;
	normal.n[0] = -16384;
	normal.dominant_axis = 4;
	model.normals.push_back(normal);
	CollisionFace face;
	face.vertex_index[0] = 0;
	face.vertex_index[1] = 1;
	face.vertex_index[2] = 2;
	face.normal_index = 0;
	face.min[1] = face.min[2] = q16(-half_extent);
	face.max[1] = face.max[2] = q16(half_extent);
	model.faces.push_back(face);
	CollisionSection section;
	section.vertex_count = 3;
	section.normal_count = 1;
	section.face_count = 1;
	model.sections.push_back(section);
	return model;
}

// The clip walks the static and vehicle pools only: a person in the way never
// clips the beam, a pool-1 item does, and the beam keeps its full range past
// everything [orig: Projectile_RaycastProximitySlots(2 / 1) @ 0x5c61ef /
// @ 0x5c6218].
void test_clip_distance() {
	auto w_heap = std::make_unique<World>();
	World &w = *w_heap;
	w.registry.configure_pool(0, 4);
	w.registry.configure_pool(1, 8);
	Entity shooter_seed;
	shooter_seed.kind = EntityKind::Organic;
	shooter_seed.position = Vec3{0.0f, 0.0f, 1.0f};
	const EntityHandle shooter = w.registry.spawn(0, shooter_seed);
	Entity bystander_seed;
	bystander_seed.kind = EntityKind::Organic;
	bystander_seed.position = Vec3{3.0f, 0.0f, 1.0f};
	bystander_seed.bound_radius = 1.0f;
	w.registry.spawn(0, bystander_seed);
	Entity crate_seed;
	crate_seed.kind = EntityKind::Item;
	crate_seed.item_id = 500;
	crate_seed.has_item_def = true;
	crate_seed.health = 40;
	crate_seed.position = Vec3{6.0f, 0.0f, 1.0f};
	crate_seed.yaw = 90; // identity model placement
	crate_seed.bound_radius = 1.0f;
	const EntityHandle crate = w.registry.spawn(1, crate_seed);
	CollisionWorld collision;
	collision.assign_entity(crate, collision.add_model(wall_model(1.0)));
	collision.build_tick_tables(w);
	w.collision = &collision;

	const int32_t origin[3] = {0, 0, q16(1.0)};
	const int32_t along_x[3] = {q16(1.0), 0, 0};
	const int32_t clip = nvg_laser_clip_distance(collision, w, shooter, origin, along_x);
	CHECK(std::abs(clip - q16(6.0)) <= 2);
	// Away from the crate the beam runs its full 8 units.
	const int32_t along_y[3] = {0, q16(1.0), 0};
	CHECK(nvg_laser_clip_distance(collision, w, shooter, origin, along_y) == kNvgLaserRangeQ16);
	// Past the range the crate never clips.
	const int32_t far_origin[3] = {q16(-3.0), 0, q16(1.0)};
	CHECK(nvg_laser_clip_distance(collision, w, shooter, far_origin, along_x) ==
			kNvgLaserRangeQ16);
}

} // namespace

int main() {
	test_gate();
	test_points_unclipped();
	test_points_clipped();
	test_clip_distance();
	if (failures == 0) std::printf("nvg_laser_test: all checks passed\n");
	return failures == 0 ? 0 : 1;
}
