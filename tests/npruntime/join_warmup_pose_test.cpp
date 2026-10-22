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

// The player class init zeroes the added body's own Pitch and Roll words, whatever the placement
// copied (a start marker's), after the model init stamped savedLivePose from them; a body whose
// row runs no such init (an item class row) keeps the placed words.
// [orig: PlayerClass_InitEntity @0x4B1060 - ebx = 0 @0x4B1074, Roll @0x4B11C4, Pitch @0x4B11CD]
bool run_the_player_class_init_zeroes_pitch_and_roll() {
	constexpr int32_t kPitch10 = 0x071C0000;                            // spawn_angle_bam(10)
	constexpr int32_t kRollMinus7 = static_cast<int32_t>(0xFB060000u); // spawn_angle_bam(-7)
	for (const bool player_row : {true, false}) {
		auto world = std::make_unique<w::World>();
		world->registry.configure_pool(0, 8);
		if (!player_row) world->tables.player.body_class = w::PlayerBodyClass::kItem;
		w::PlayerSpawn spawn;
		spawn.position = {8.0f, 8.0f, 0.0f};
		spawn.team = 1;
		spawn.pitch_bam = w::spawn_angle_bam(10);
		spawn.roll_bam = w::spawn_angle_bam(-7);
		if (!expect(spawn.pitch_bam == kPitch10 && spawn.roll_bam == kRollMinus7,
		            "the marker's words")) return false;
		const w::EntityHandle h = w::spawn_remote_player(*world, spawn);
		const w::Entity *e = world->registry.get(h);
		if (!expect(e != nullptr, "the add spawns the body")) return false;
		if (player_row) {
			if (!expect(e->pitch_bam == 0 && e->roll_bam == 0 && e->saved_live_pitch == 0 &&
			                    e->saved_live_roll == 0,
			            "the player class init zeroes Pitch / Roll and their saved words"))
				return false;
		} else {
			if (!expect(e->pitch_bam == kPitch10 && e->roll_bam == kRollMinus7,
			            "an item class row keeps the placed Pitch / Roll words")) return false;
		}
	}
	return true;
}

// A pool-2 building on the body's own spot whose one volume of `type` has its -X face a quarter
// unit past the body's column, the other faces 20 units off: every warm-up test point (radii
// 0.5625 / 0.3125 / 0.5 for the double's one-unit capsule) reaches into it.
w::CollisionModel wall_model(int32_t type) {
	w::CollisionModel model;
	const auto plane = [&](int16_t x, int16_t y, int16_t z, int32_t dist) {
		w::CollisionPlane p;
		p.nx = x;
		p.ny = y;
		p.nz = z;
		p.dist = dist;
		model.planes.push_back(p);
	};
	plane(-16384, 0, 0, kOne / 4);
	plane(16384, 0, 0, -20 * kOne);
	plane(0, 16384, 0, -20 * kOne);
	plane(0, -16384, 0, -20 * kOne);
	plane(0, 0, 16384, -20 * kOne);
	plane(0, 0, -16384, -20 * kOne);
	w::CollisionVolume volume;
	volume.type = type;
	volume.min_x = kOne / 4;
	volume.max_x = 20 * kOne;
	volume.min_y = volume.min_z = -20 * kOne;
	volume.max_y = volume.max_z = 20 * kOne;
	volume.plane_count = 6;
	model.volumes.push_back(volume);
	w::CollisionSection section;
	section.volume_count = 1;
	model.sections.push_back(section);
	return model;
}

// The join's warm-up resolve takes the player bit from the body's own Flags (0x100), as every
// per-tick resolve does: a player body meets a player-only (CP) volume as it meets a solid (CB)
// one, a body without the bit passes through the CP volume and still meets the CB one.
// [orig: Entity_MovementCollisionResolver @0x4B2BD0 - (Flags >> 8) & 1 @0x4B2C10..0x4B2C13,
//  the query mask `lea ecx, [edx+ecx*2]` @0x4B2F79; Entity_ComputeBoneCollisionForce - the CP
//  volume's mask test @0x4AE543]
bool run_the_warmup_resolve_takes_the_player_bit() {
	int32_t x[2][2] = {};
	for (const bool player : {true, false}) {
		for (const bool cp : {true, false}) {
			Rig r;
			w::Entity &e = r.entity();
			// The +0x1C word the pool-0 slice build gates on, and a bound radius for the
			// contact clamp.
			e.item_type_index = 3;
			e.bound_radius = 1;
			if (!player) {
				e.flags &= ~w::kEntityFlagPlayer;
				e.engine_flags &= ~w::kEntityFlagPlayer;
			}
			r.world->registry.configure_pool(2, 1);
			w::Entity building;
			building.kind = w::EntityKind::Building;
			building.position = {8.0f, 8.0f, 0.0f};
			building.heading = w::placed_heading_bam(90);
			building.bound_radius = 30;
			building.alive = true;
			const w::EntityHandle wall = r.world->registry.spawn(2, building);
			r.collision->assign_entity(wall, r.collision->add_model(wall_model(
					cp ? w::bvol_type::kPlayerCP : w::bvol_type::kSolidCB)));
			r.rebuild();
			w::warm_up_player_body(*r.world, e);
			x[player ? 0 : 1][cp ? 0 : 1] = e.position.x.q16;
		}
	}
	if (!expect(x[0][1] < 8 * kOne && x[1][1] == x[0][1],
	            "the CB volume pushes the body off the face, player bit or not")) return false;
	if (!expect(x[0][0] == x[0][1], "a player body meets the CP volume as it meets the CB one"))
		return false;
	return expect(x[1][0] == 8 * kOne, "a body without the player bit passes the CP volume");
}

} // namespace

int main() {
	int failures = 0;
	if (!run_the_add_stamps_the_placed_pose()) ++failures;
	if (!run_the_join_warmup_reads_the_add_time_words()) ++failures;
	if (!run_the_skip_test_reads_the_stamp()) ++failures;
	if (!run_the_player_class_init_zeroes_pitch_and_roll()) ++failures;
	if (!run_the_warmup_resolve_takes_the_player_bit()) ++failures;
	if (failures != 0) return 1;
	std::printf("OK\n");
	return 0;
}
