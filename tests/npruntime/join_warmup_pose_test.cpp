// npruntime_join_warmup_pose: the join's warm-up resolve over the words the add left (net parity
// lane L1, D-NET-503's late-join residue). A retail joiner joining our host after another player
// takes TDH_I3A's 6003 start marker beside two buildings, where the warm-up's resolver pushes its
// body before the S2C 0x0F carries it; our push differed by one unit on X and Y. The resolver
// reads two of the body's words as the add left them, since a hidden body runs no mover until its
// join:
//   - savedLivePose (+0x80..+0x88), which the add's model init stamps from the placed words and
//     the join's placement leaves, gating the planes, the skip test and the run-over delta
//     [orig: Entity_SpawnFromAnimSlotProperty @0x43C390 - the placement @0x43C459, then
//     Entity_InitFromModel @0x43C492 (the stamp @0x40E14D..0x40E169);
//     Entity_ComputeBoneCollisionForce @0x4AE3AC; Entity_MovementCollisionResolver @0x4B2BD0 -
//     the skip test's X leg @0x4B2C72..0x4B2C82, its Y leg the equality bit @0x4B2C84..0x4B2C99];
//   - CameraOffset (+0x6C..+0x74), the eye test point, which only the mover writes, so the add's
//     memset 0 stands [orig: the read @0x4B2EE2..0x4B2EFB; the memset @0x43C3FE].

#include <runtime/terrain_query/height_field.h>
#include <runtime/world/collision.h>
#include <runtime/world/entity.h>
#include <runtime/world/entity_spawn.h>
#include <runtime/world/infantry.h>
#include <runtime/world/player_spawn.h>
#include <runtime/world/world.h>

#include <cstdint>
#include <cstdio>
#include <memory>
#include <set>
#include <vector>

namespace {

namespace w = opennova::world;

bool expect(bool cond, const char *msg) {
	if (cond) return true;
	std::fprintf(stderr, "FAIL: %s\n", msg);
	return false;
}

constexpr int32_t kOne = 65536;

// A flat heightfield at 0 (the collision tests' field).
struct Field {
	static constexpr int kDim = 512;
	std::vector<uint16_t> heightmap;
	std::vector<int> sector_grid;
	opennova::terrain::TerrainHeightField field;
	Field() : heightmap(kDim * kDim, 0), sector_grid(256, 1) {
		field.heightmap = heightmap.data();
		field.dim = kDim;
		field.layout.sector_grid = sector_grid.data();
		field.layout.origin_x = 0;
		field.layout.origin_y = 0;
	}
};

// A root-motion double holding the reset clip: a one-unit capsule and no motion.
struct Source final : w::IRootMotionSource {
	std::set<int> clips{0, 1, 27, 28, 29, 43, 44, 67, 68, 75, 76, 140};
	bool has_clip(int, int state) const override { return clips.count(state) != 0; }
	int32_t clip_length_ticks(int, int, int) const override { return 10000; }
	bool advance(int, int state, int32_t &phase, w::RootMotionFrame &out) override {
		if (!has_clip(0, state)) return false;
		++phase;
		out = {};
		out.capsule_bottom = kOne;
		out.capsule_top = 2 * kOne;
		return true;
	}
};

// A remote player's add at (8, 8, 0) on a world with a collision world and a flat ground. The
// World and the collision world are heap-held (the 1 MB test-main stack under MSVC).
struct Rig {
	std::unique_ptr<w::World> world = std::make_unique<w::World>();
	std::unique_ptr<w::CollisionWorld> collision = std::make_unique<w::CollisionWorld>();
	Field field;
	Source source;
	w::EntityHandle body;

	Rig() {
		world->registry.configure_pool(0, 8);
		collision->terrain = &field.field;
		world->ai.collision = collision.get();
		world->ai.root_motion = &source;
		w::PlayerSpawn spawn;
		spawn.position = {8.0f, 8.0f, 0.0f};
		spawn.team = 1;
		body = w::spawn_remote_player(*world, spawn);
	}

	w::Entity &entity() { return *world->registry.get(body); }

	// The candidate slices rebuild on the 17-tick edge.
	void rebuild() {
		for (int i = 0; i < 17; ++i) collision->build_tick_tables(*world);
	}
};

// The add's model init stamps savedLivePose from the placed words, and the body's CameraOffset
// is the memset's 0.
bool run_the_add_stamps_the_placed_pose() {
	Rig r;
	if (!expect(r.body.valid(), "the add spawns the body")) return false;
	const w::Entity &e = r.entity();
	if (!expect(e.saved_live_valid && e.saved_live_pos[0] == 8 * kOne &&
	                    e.saved_live_pos[1] == 8 * kOne && e.saved_live_pos[2] == 0,
	            "the add's model init stamps savedLivePose from the placed words")) return false;
	const w::AiEntity *ai = r.world->ai.for_handle(r.body);
	return expect(ai != nullptr && ai->inf.eye_offset_x == 0 && ai->inf.eye_offset_y == 0 &&
	                      ai->inf.eye_offset_z == 0,
	              "the add's body holds CameraOffset (0, 0, 0)");
}

// The join places the body on another marker and warms it up: the resolve tests the eye point at
// the body's origin (CameraOffset 0, never the derived capsule height), and the add's stamp
// stands through it.
bool run_the_join_warmup_reads_the_add_time_words() {
	Rig r;
	w::Entity &e = r.entity();
	e.position.set_q16({12 * kOne, 8 * kOne, 0}); // the join's placement
	r.rebuild();
	r.collision->local_player = r.body; // the resolver's capture of its test points
	w::warm_up_player_body(*r.world, e);
	const w::CollisionWorld::LocalResolveDebug &debug = r.collision->local_resolve_debug;
	if (!expect(debug.valid, "the warm-up runs a full resolve")) return false;
	if (!expect(debug.points[1][0] == debug.points[2][0] &&
	                    debug.points[1][1] == debug.points[2][1] &&
	                    debug.points[1][2] == debug.points[2][2],
	            "the eye test point is the body's origin: CameraOffset (0, 0, 0)")) return false;
	return expect(e.saved_live_pos[0] == 8 * kOne && e.saved_live_pos[1] == 8 * kOne &&
	                      e.saved_live_pos[2] == 0,
	              "the join's placement and warm-up leave the add's stamp");
}

// The skip test reads the stamp, not the resolver's last pose: an X displacement past 200 from
// it forces the full update; a Y displacement alone does not (retail's Y leg tests the equality
// bit, 0 or 1).
bool run_the_skip_test_reads_the_stamp() {
	for (const bool along_x : {true, false}) {
		Rig r;
		r.rebuild();
		w::Entity &e = r.entity();
		int32_t pos[3] = {8 * kOne, 8 * kOne, 0};
		e.saved_live_pos[0] = along_x ? pos[0] - 300 : pos[0];
		e.saved_live_pos[1] = along_x ? pos[1] : pos[1] - 300;
		e.saved_live_pos[2] = 0;
		e.saved_live_valid = true;
		w::CollisionWorld::ResolveState state; // its last pose: where the body stands
		state.prev_pos[0] = pos[0];
		state.prev_pos[1] = pos[1];
		state.prev_pos[2] = pos[2];
		state.prev_valid = true;
		state.skip_counter = 15; // inside the skip band
		int32_t vel[3] = {0, 0, 0};
		int16_t health = 100;
		r.world->entity_update_counter = 13;
		(void)r.collision->resolve_entity(*r.world, r.body, state, pos, vel, vel[2], kOne,
				2 * kOne, 0, 0, /*is_player_class=*/true, /*is_authority=*/true, 13, 43, 0u,
				health);
		if (along_x) {
			if (!expect(state.skip_counter == 0,
			            "an X displacement past 200 from the stamp forces the full update"))
				return false;
		} else {
			if (!expect(state.skip_counter == 16,
			            "a Y displacement alone keeps the skip band")) return false;
		}
	}
	return true;
}

} // namespace

int main() {
	int failures = 0;
	if (!run_the_add_stamps_the_placed_pose()) ++failures;
	if (!run_the_join_warmup_reads_the_add_time_words()) ++failures;
	if (!run_the_skip_test_reads_the_stamp()) ++failures;
	if (failures != 0) return 1;
	std::printf("OK\n");
	return 0;
}
