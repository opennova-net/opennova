// The Stats window's row tree against the laps the tick really takes: the
// synthetic mission ticks through the bare local role with the kernel's one
// TickProfile active, and every tick's touched slots must fit the tree the
// window draws (stats_window_rows.h). A lap that moves inside another span
// without the tree following it shows up here as a child larger than its
// parent or a negative "unattributed" row, which the window would otherwise
// print as nonsense.
#include "common/boot_file_source.h"
#include "common/synthetic_mission.h"

#include <runtime/devtools/stats_window_rows.h>
#include <runtime/devtools/tick_profile.h>
#include <runtime/inmatch/local_role.h>
#include <runtime/mission/mission_kernel.h>

#include <cstdio>
#include <cstring>
#include <map>
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
			Slot::SIM_UPDATE_EXPLOSIONS, Slot::SIM_UPDATE_PROXIMITY, Slot::SIM_WORLD_HOUSEKEEPING,
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

void test_row_tree_shape() {
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
	if (g_failures != 0) {
		std::printf("%d failure(s)\n", g_failures);
		return 1;
	}
	std::printf("tick_profile_rows: ok\n");
	return 0;
}
