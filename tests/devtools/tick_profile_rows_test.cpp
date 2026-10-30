// The Stats window's row tree against the laps the tick really takes: the
// synthetic mission ticks through the bare local role with the kernel's one
// TickProfile active, and every tick's touched slots must fit the tree the
// window draws (stats_window_rows.h). A lap that moves inside another span
// without the tree following it shows up here as a child larger than its
// parent or a negative "unattributed" row, which the window would otherwise
// print as nonsense. An NPC's and a player body's update laps take the same
// test on a bare World, each body holding its animation channel.
#include "common/boot_file_source.h"
#include "common/synthetic_mission.h"
#include "world/channel_clip_source.h"

#include <runtime/devtools/stats_window_rows.h>
#include <runtime/devtools/tick_profile.h>
#include <runtime/inmatch/local_role.h>
#include <runtime/mission/mission_kernel.h>
#include <runtime/world/ai.h>
#include <runtime/world/world.h>

#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <string>

using namespace opennova;
using devtools::RowKind;
using devtools::Slot;
using devtools::StatsRow;
namespace rows = devtools::stats_rows;

namespace {

int g_failures = 0;

#define CHECK(cond, ...)                                          \
	do {                                                          \
		if (!(cond)) {                                            \
			std::printf("FAIL %s:%d: ", __FILE__, __LINE__);      \
			std::printf(__VA_ARGS__);                             \
			std::printf("\n");                                    \
			++g_failures;                                         \
		}                                                         \
	} while (0)

constexpr int kTicks = 240;

// The row a row nests under: the nearest shallower row above it.
int parent_of(int index) {
	const int depth = rows::kRows[index].depth;
	for (int i = index - 1; i >= 0; --i)
		if (rows::kRows[i].depth < depth) return i;
	return -1;
}

int find_row(const char *id) {
	for (int i = 0; i < rows::kRowCount; ++i)
		if (std::strcmp(rows::kRows[i].id, id) == 0) return i;
	return -1;
}

// Checks one tick's profile against the tree: every touched SPAN child fits
// inside its touched SPAN parent, and every RESIDUAL over a touched base
// stays non-negative.
void check_tick(const devtools::TickProfile &profile, int tick) {
	for (int i = 0; i < rows::kRowCount; ++i) {
		const StatsRow &row = rows::kRows[i];
		if (row.kind == RowKind::SPAN && profile.touched(row.slot)) {
			const int parent = parent_of(i);
			if (parent >= 0 && rows::kRows[parent].kind == RowKind::SPAN &&
					profile.touched(rows::kRows[parent].slot)) {
				CHECK(profile.sum(row.slot) <= profile.sum(rows::kRows[parent].slot),
						"tick %d: '%s' (%lld us) exceeds its parent '%s' (%lld us)", tick, row.id,
						static_cast<long long>(profile.sum(row.slot)), rows::kRows[parent].id,
						static_cast<long long>(profile.sum(rows::kRows[parent].slot)));
			}
		}
		if (row.kind == RowKind::RESIDUAL && profile.touched(row.slot)) {
			int64_t residual = profile.sum(row.slot);
			for (int k = 0; k < row.slot_count; ++k) residual -= profile.sum(row.slots[k]);
			CHECK(residual >= 0, "tick %d: residual '%s' is negative (%lld us)", tick, row.id,
					static_cast<long long>(residual));
		}
	}
}

void test_local_role_ticks_fit_the_row_tree() {
	std::map<std::string, std::string> files;
	files["synth.wac"] = "if elapse(1) then set(v1,1) endif\nif elapse(2) then inc(v1) endif\n";
	mission::MissionKernel kernel;
	kernel.open_document(test_mission::synthetic_mission(), "synth",
			test_boot::source_over(&files));
	std::string error;
	mission::KernelBootOptions options;
	if (!kernel.boot(options, error)) {
		CHECK(false, "the synthetic mission boots: %s", error.c_str());
		return;
	}
	CHECK(kernel.world.profile == &kernel.profile, "the world laps onto the kernel's profile");
	kernel.profile.set_active(true);
	inmatch::LocalRole role;
	role.bind(kernel);

	const Slot kExpected[] = {
			Slot::SIM_SERVER_WORLD, Slot::SIM_WORLD_SETUP, Slot::SIM_WORLD_SCRIPTS,
			Slot::SIM_UPDATE_ENTITIES, Slot::SIM_UPDATE_WALKS, Slot::SIM_UPDATE_ATTACHMENTS,
			Slot::SIM_UPDATE_HELILIFT_FACES, Slot::SIM_UPDATE_PRECIPITATION,
			Slot::SIM_UPDATE_PIECES_EVENTS, Slot::SIM_UPDATE_PROJECTILES,
			Slot::SIM_UPDATE_EXPLOSIONS, Slot::SIM_UPDATE_PROXIMITY, Slot::SIM_UPDATE_CONTACTS,
			Slot::SIM_UPDATE_EPILOG, Slot::SIM_UPDATE_BODY_TRACE, Slot::SIM_WORLD_HOUSEKEEPING,
			Slot::SIM_PLAYER_TAIL, Slot::SIM_WEAPON_WALK, Slot::SIM_ADM_RESOLVE};
	bool seen[sizeof(kExpected) / sizeof(kExpected[0])] = {};
	for (int tick = 0; tick < kTicks; ++tick) {
		kernel.profile.reset();
		role.run_tick(inmatch::TickInput{});
		check_tick(kernel.profile, tick);
		for (size_t k = 0; k < sizeof(kExpected) / sizeof(kExpected[0]); ++k)
			seen[k] = seen[k] || kernel.profile.touched(kExpected[k]);
	}
	for (size_t k = 0; k < sizeof(kExpected) / sizeof(kExpected[0]); ++k)
		CHECK(seen[k], "the local role's tick touches %s",
				devtools::slot_name(kExpected[k]));
}

// One organic body on a bare World with the profile active: an NPC (org1) or a remote
// player's body on the authority (org2), holding its channel and playing nothing.
struct BodyRig {
	test_world::ChannelClipSource channel_only;
	devtools::TickProfile profile;
	std::unique_ptr<world::World> w = std::make_unique<world::World>();
	world::EntityHandle body;

	explicit BodyRig(bool player) {
		w->registry.configure_pool(0, 4);
		world::Entity seed;
		seed.net_id = 7;
		seed.kind = world::EntityKind::Organic;
		seed.health = seed.health_max = 100;
		seed.alive = true;
		if (player) seed.flags |= world::kEntityFlagPlayer;
		body = w->registry.spawn(0, seed);
		w->ai.root_motion = &channel_only;
		w->ai.is_authority = true;
		world::AiEntity *e = w->ai.at(w->ai.attach(body));
		e->net_id = seed.net_id;
		e->health = 100;
		e->inf.active = true;
		e->inf.adm_id = 1;
		e->inf.max_health = 100;
		profile.set_active(true);
		w->profile = &profile;
	}
};

// The body update's one lap (tick_infantry) against the tree: every tick fits it (no
// child past its parent, no negative "Infantry remainder"), each body's legs land on
// their rows, and the other body's own legs never do.
void run_body_laps(bool player, const Slot *expected, int count, Slot never) {
	BodyRig rig(player);
	bool seen[16] = {};
	bool never_seen = false;
	for (uint32_t tick = 1; tick <= 64; ++tick) {
		rig.profile.reset();
		world::AiEntity *e = rig.w->ai.for_handle(rig.body);
		if (e == nullptr) {
			CHECK(false, "the rig's body stays attached");
			return;
		}
		rig.w->ai.update_organic(*e, *rig.w, tick,
				world::body_move_row(*rig.w, *e, rig.w->registry.get(rig.body)));
		check_tick(rig.profile, static_cast<int>(tick));
		CHECK(rig.profile.touched(Slot::SIM_AI_INFANTRY), "tick %u: the update spans its row", tick);
		for (int k = 0; k < count; ++k) seen[k] = seen[k] || rig.profile.touched(expected[k]);
		never_seen = never_seen || rig.profile.touched(never);
	}
	for (int k = 0; k < count; ++k)
		CHECK(seen[k], "the %s body's update touches %s", player ? "player" : "NPC",
				devtools::slot_name(expected[k]));
	CHECK(!never_seen, "the %s body's update never touches %s", player ? "player" : "NPC",
			devtools::slot_name(never));
}

void test_body_laps_fit_the_row_tree() {
	const Slot kNpc[] = {Slot::SIM_AI_INFANTRY_ANIMATION, Slot::SIM_AI_INFANTRY_CORPSE,
			Slot::SIM_AI_INFANTRY_MOTION, Slot::SIM_AI_INFANTRY_THINK, Slot::SIM_AI_INFANTRY_FIRE,
			Slot::SIM_AI_INFANTRY_FINISH};
	run_body_laps(false, kNpc, 6, Slot::SIM_AI_INFANTRY_PLAYER);
	const Slot kPlayer[] = {Slot::SIM_AI_INFANTRY_ANIMATION, Slot::SIM_AI_INFANTRY_PLAYER,
			Slot::SIM_AI_INFANTRY_CORPSE, Slot::SIM_AI_INFANTRY_MOTION, Slot::SIM_AI_INFANTRY_FIRE,
			Slot::SIM_AI_INFANTRY_FINISH};
	run_body_laps(true, kPlayer, 6, Slot::SIM_AI_INFANTRY_THINK);
}

// A remainder row subtracts exactly the SPAN rows beside it under its base row: a lap
// added under the base without its slot in the list (or a stale slot left in it) prints a
// wrong remainder.
void check_remainder_lists_its_siblings(const char *id) {
	const int row = find_row(id);
	if (row < 0) {
		CHECK(false, "the remainder row '%s' exists", id);
		return;
	}
	const int base = parent_of(row);
	if (base < 0 || rows::kRows[base].slot != rows::kRows[row].slot) {
		CHECK(false, "'%s' is its parent row's remainder", id);
		return;
	}
	const int depth = rows::kRows[row].depth;
	int siblings = 0;
	for (int i = base + 1; i < rows::kRowCount && rows::kRows[i].depth > rows::kRows[base].depth; ++i) {
		if (rows::kRows[i].depth != depth || rows::kRows[i].kind != RowKind::SPAN) continue;
		++siblings;
		bool listed = false;
		for (int k = 0; k < rows::kRows[row].slot_count; ++k)
			listed = listed || rows::kRows[row].slots[k] == rows::kRows[i].slot;
		CHECK(listed, "'%s' subtracts its sibling '%s'", id, rows::kRows[i].id);
	}
	CHECK(rows::kRows[row].slot_count == siblings, "'%s' lists %d slots for its %d siblings", id,
			rows::kRows[row].slot_count, siblings);
}

void test_row_tree_shape() {
	check_remainder_lists_its_siblings("ai_infantry_unattributed");
	check_remainder_lists_its_siblings("update_walks_other");
	check_remainder_lists_its_siblings("update_entities_unattributed");
	check_remainder_lists_its_siblings("attachment_unattributed");
	// The attached child's compose runs inside the ewep class update, under its row.
	const int compose = find_row("walk_ewep_compose");
	CHECK(compose >= 0 && std::strcmp(rows::kRows[parent_of(compose)].id, "walk_ewep") == 0,
			"the attached-child compose nests under the ewep class update");
	// The frame tail sits under the sim step beside the role pumps, outside
	// the world update; the entity update's laps nest under it.
	const int weapon_walk = find_row("weapon_walk");
	const int player_tail = find_row("player_tail");
	CHECK(weapon_walk >= 0 && rows::kRows[weapon_walk].depth == 4, "weapon_walk is a sim-step row");
	CHECK(player_tail >= 0 && rows::kRows[player_tail].depth == 4, "player_tail is a sim-step row");
	const int update = find_row("update_entities");
	CHECK(update >= 0 && std::strcmp(rows::kRows[parent_of(update)].id, "server_world") == 0,
			"the entity update nests under the world update");
	const int explosions = find_row("update_explosions");
	CHECK(explosions >= 0 && parent_of(explosions) == update,
			"the explosion lap nests under the entity update");
	// The session frame's phases sit beside the sim step under the session leg, and its
	// remainder is what is past all four.
	const char *kPhases[] = {"session_frame_bank", "update_input_frames",
			"session_frame_render_legs", "session_frame_marshal", "session_frame_other"};
	for (const char *id : kPhases) {
		const int row = find_row(id);
		CHECK(row >= 0 && rows::kRows[row].depth == rows::kRows[find_row("sim")].depth,
				"'%s' is a session-leg row beside the sim step", id);
	}
	const char *kInputParts[] = {"update_input_rows", "update_input_drain", "update_input_apply",
			"update_input_other"};
	for (const char *id : kInputParts) {
		const int row = find_row(id);
		CHECK(row >= 0 && std::strcmp(rows::kRows[parent_of(row)].id, "update_input_frames") == 0,
				"'%s' nests under the updates' input frames", id);
	}
	const int other = find_row("session_frame_other");
	CHECK(other >= 0 && rows::kRows[other].slot == Slot::SIM_ADVANCE &&
					rows::kRows[other].slot_count == 4,
			"the session frame's remainder is SIM_ADVANCE past its four phases");
	const char *kRetired[] = {"world_ai", "ai_other_entities", "ai_auth_vehicles",
			"ai_client_vehicles", "world_throwables", "world_weapons", "world_destruction",
			"host_player"};
	for (const char *id : kRetired) CHECK(find_row(id) < 0, "the retired row '%s' is gone", id);
	for (int i = 0; i < rows::kRowCount; ++i) {
		const StatsRow &row = rows::kRows[i];
		if (row.kind == RowKind::SPAN || row.kind == RowKind::RESIDUAL)
			CHECK(row.slot != Slot::COUNT, "row '%s' names a slot", row.id);
		if (row.kind == RowKind::GROUP || row.kind == RowKind::RESIDUAL)
			CHECK(row.slots != nullptr && row.slot_count > 0, "row '%s' lists its slots", row.id);
	}
}

}  // namespace

int main() {
	test_row_tree_shape();
	test_local_role_ticks_fit_the_row_tree();
	test_body_laps_fit_the_row_tree();
	if (g_failures != 0) {
		std::printf("%d failure(s)\n", g_failures);
		return 1;
	}
	std::printf("tick_profile_rows: ok\n");
	return 0;
}
