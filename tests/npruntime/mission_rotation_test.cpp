// The host's map rotation (D-NET-331, net-re §5.70): the list ops and their
// cursor rules (inmatch/mission_rotation.h), the admin console's insert and
// one-shot arm and their seam (inmatch/rotation_admin.h, ADR 0051 PR5b), the
// round-end advance with the REPLAY wrap, the host screen's START seed, the round end's exit reason
// under REPLAY and LASTGAME, and the map change's router over the
// Attack-and-Defend halves and the SETNEXT latch (inmatch/map_change.h).
#include <base/gameprofile/game_type.h>
#include <formats/mission/bms.h>
#include <runtime/inmatch/host_role.h>
#include <runtime/inmatch/map_change.h>
#include <runtime/inmatch/mission_exit.h>
#include <runtime/inmatch/mission_rotation.h>
#include <runtime/inmatch/rotation_admin.h>
#include <runtime/inmatch/server_spawn.h>
#include <runtime/inmatch/server_tick.h>
#include <runtime/world/world.h>

#include <cstdio>
#include <vector>

using namespace opennova;
using namespace opennova::inmatch;

namespace {

int failures = 0;
#define CHECK(c)                                                                     \
	do {                                                                             \
		if (!(c)) {                                                                  \
			std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c);                 \
			++failures;                                                              \
		}                                                                            \
	} while (0)

mission_catalog::Row row(const char *file, bms::AttribFlags mode, bool loose = false) {
	mission_catalog::Row r;
	r.file = file;
	r.game_mode = static_cast<uint32_t>(mode);
	r.loose = loose;
	return r;
}

// Row 0 a team map (TDM), row 1 an objective co-op map, row 2 a non-team map
// (DM), row 3 a team map (CTF) loaded loose.
std::vector<mission_catalog::Row> catalog() {
	return {
		row("TDM_A.BMS", bms::AttribFlags::TeamDeathmatch),
		row("COOP_B.BMS", bms::AttribFlags::Coop),
		row("DM_C.BMS", bms::AttribFlags::Deathmatch),
		row("CTF_D.BMS", bms::AttribFlags::CaptureTheFlag, /*loose=*/true),
	};
}

// The list a seed of `rows` leaves, every row's launch option set first.
MissionRotation seeded(const std::vector<mission_catalog::Row> &cat,
		const std::vector<int32_t> &rows) {
	HostRotation r;
	seed_rotation_from_host_screen(r, cat, rows, std::vector<int32_t>(cat.size(), 1));
	return r.list;
}

void test_alloc_append_find() {
	const auto cat = catalog();
	MissionRotation list;
	// The alloc: the min-1 capacity, count 0, cursor -1, the memset's alt 0.
	list.alloc(0);
	CHECK(list.allocated && list.capacity() == 1 && list.count == 0);
	CHECK(list.cursor == -1 && list.alt_cursor == 0);
	CHECK(list.current() == nullptr);
	// An index outside [0, catalog count) appends nothing.
	list.append(cat, -1, 0);
	list.append(cat, static_cast<int32_t>(cat.size()), 0);
	CHECK(list.count == 0);
	// The launch option survives only on a team, non-objective row.
	for (size_t i = 0; i < cat.size(); ++i) list.launch_option(cat, static_cast<int32_t>(i)) = 1;
	list.append(cat, 0, 0);
	CHECK(list.count == 1 && list.alt_cursor == 0); // no regrow: the alt cursor stays
	list.append(cat, 1, 0);                          // the regrow by five
	CHECK(list.capacity() == 6 && list.count == 2 && list.alt_cursor == -1);
	list.append(cat, 2, 0);
	list.append(cat, 3, 0);
	CHECK(list.launch_options[0] == 1 && list.launch_options[1] == 0 &&
			list.launch_options[2] == 0 && list.launch_options[3] == 1);
	// A null list allocates lazily at capacity 1.
	MissionRotation lazy;
	lazy.append(cat, 2, 0);
	CHECK(lazy.allocated && lazy.capacity() == 1 && lazy.count == 1 && lazy.cursor == -1);
	// FindByName: case-insensitive, the first match, the alt cursor cleared.
	CHECK(list.set_alt_cursor(3));
	list.find_by_name(cat, "dm_c.bms");
	CHECK(list.cursor == 2 && list.alt_cursor == -1);
	CHECK(list.current() == &list.entry(2));
	list.find_by_name(cat, "NOSUCH.BMS");
	CHECK(list.cursor == -1 && list.current() == nullptr);
	// The walk runs to the capacity: a zeroed slot past the count names
	// catalog row 0 [orig: MissionList_FindByName @0x4FC4F0..0x4FC51B].
	MissionRotation short_list;
	short_list.alloc(3);
	short_list.append(cat, 2, 0);
	short_list.find_by_name(cat, "TDM_A.BMS");
	CHECK(short_list.cursor == 1 && short_list.current() == nullptr);
}

void test_remove_alt_reset_free() {
	const auto cat = catalog();
	MissionRotation list = seeded(cat, {0, 1, 2, 3});
	CHECK(list.count == 4 && list.cursor == 0);
	CHECK(!list.remove_entry(-1) && !list.remove_entry(4));
	CHECK(list.set_alt_cursor(3) && !list.set_alt_cursor(4) && !list.set_alt_cursor(-1));
	list.cursor = 2;
	// Removing index 1: the tail shifts down, both cursors past it step down.
	CHECK(list.remove_entry(1));
	CHECK(list.count == 3 && list.entry(0).catalog_index == 0 && list.entry(1).catalog_index == 2 &&
			list.entry(2).catalog_index == 3);
	CHECK(list.cursor == 1 && list.alt_cursor == 2);
	// A cursor at the index stays.
	CHECK(list.remove_entry(1));
	CHECK(list.cursor == 1 && list.alt_cursor == 1 && list.count == 2);
	list.reset_cursor();
	CHECK(list.cursor == -1 && list.count == 0 && list.allocated);
	list.free_buffer();
	CHECK(!list.allocated && list.count == 0 && !list.set_alt_cursor(0) && !list.remove_entry(0));
}

void test_advance() {
	const auto cat = catalog();
	// A null or an empty list has no next mission.
	MissionRotation none;
	CHECK(!none.advance(cat, true));
	MissionRotation empty;
	empty.alloc(4);
	CHECK(!empty.advance(cat, true));

	// A clear flag steps by one and copies the row out.
	MissionRotation list = seeded(cat, {0, 2, 3});
	CHECK(list.map_file == "TDM_A.BMS" && list.map_launch_option == 1);
	CHECK(list.advance(cat, false));
	CHECK(list.cursor == 1 && list.map_file == "DM_C.BMS" && !list.map_source_is_loose);
	CHECK(list.map_launch_option == 0 && list.map_game_type == game_type::kDeathmatch);
	CHECK(list.advance(cat, false));
	CHECK(list.cursor == 2 && list.map_file == "CTF_D.BMS" && list.map_source_is_loose);
	CHECK(list.map_launch_option == 1 && list.map_game_type == game_type::kCaptureTheFlag);
	// Past the last entry with REPLAY off: the rotation is exhausted and the
	// outputs keep the last map.
	CHECK(!list.advance(cat, false));
	CHECK(list.cursor == 3 && list.map_file == "CTF_D.BMS");
	// A cursor at or past the count starts over at the head of the next call.
	CHECK(list.advance(cat, false));
	CHECK(list.cursor == 1 && list.map_file == "DM_C.BMS");

	// REPLAY wraps the cursor past the end to the first entry.
	MissionRotation replay = seeded(cat, {0, 2});
	CHECK(replay.advance(cat, true) && replay.cursor == 1);
	CHECK(replay.advance(cat, true) && replay.cursor == 0 && replay.map_file == "TDM_A.BMS");

	// The alt cursor is taken (the SETNEXT entry) and cleared.
	MissionRotation alt = seeded(cat, {0, 2, 3});
	CHECK(alt.set_alt_cursor(2));
	CHECK(alt.advance(cat, false));
	CHECK(alt.cursor == 2 && alt.alt_cursor == -1 && alt.map_file == "CTF_D.BMS");
	// An alt cursor at the current entry replays it.
	CHECK(alt.set_alt_cursor(2) && alt.advance(cat, false) && alt.cursor == 2);

	// With no cursor (the admin console's CLEAR then ADD) the flag read is the
	// header's alt-cursor word: 0 reads clear, and the step takes the alt cursor.
	MissionRotation unseeded;
	unseeded.append(cat, 2, 0);
	CHECK(unseeded.cursor == -1 && unseeded.alt_cursor == 0);
	CHECK(unseeded.advance(cat, false));
	CHECK(unseeded.cursor == 0 && unseeded.map_file == "DM_C.BMS");
}

// The advance's one-shot arm (MISSION ADD ... ONESHOT): the flagged entry under the cursor is
// removed in place, the entry that slid into its slot plays with no cursor move, the alt
// cursor is not taken (a pending SETNEXT waits one more map) and steps down past the cursor;
// every later entry is kept (D-NET-364: retail's copy is one entry short).
void test_one_shot() {
	const auto cat = catalog();
	// [TDM_A, COOP_B*, DM_C, CTF_D], the cursor on COOP_B*.
	MissionRotation list = seeded(cat, {0, 1, 2, 3});
	list.slots[1].flag = 1;
	list.cursor = 1;
	CHECK(list.set_alt_cursor(3));
	CHECK(list.advance(cat, false));
	CHECK(list.count == 3 && list.cursor == 1);
	CHECK(list.entry(0).catalog_index == 0 && list.entry(1).catalog_index == 2 &&
			list.entry(2).catalog_index == 3);
	CHECK(list.map_file == "DM_C.BMS");
	CHECK(list.alt_cursor == 2); // stepped down, not taken
	// The next advance takes the pending alt cursor.
	CHECK(list.advance(cat, false) && list.cursor == 2 && list.map_file == "CTF_D.BMS" &&
			list.alt_cursor == -1);

	// A one-shot LAST entry: the count drops, the cursor is past the end, and REPLAY decides
	// (retail's copy of -8 bytes runs off the heap here).
	MissionRotation last = seeded(cat, {0, 2});
	last.slots[1].flag = 1;
	last.cursor = 1;
	MissionRotation last_replay = last;
	CHECK(!last.advance(cat, false) && last.count == 1);
	CHECK(last_replay.advance(cat, true) && last_replay.count == 1 && last_replay.cursor == 0 &&
			last_replay.map_file == "TDM_A.BMS");

	// A one-shot only entry empties the list: the rotation ends even under REPLAY.
	MissionRotation only;
	only.append(cat, 0, 1);
	only.find_by_name(cat, "TDM_A.BMS");
	CHECK(only.cursor == 0 && !only.advance(cat, true) && only.count == 0 && only.allocated);
	CHECK(!only.advance(cat, true));

	// An alt cursor at or before the cursor stays.
	MissionRotation before = seeded(cat, {0, 1, 2});
	before.slots[2].flag = 1;
	before.cursor = 2;
	CHECK(before.set_alt_cursor(0));
	CHECK(before.advance(cat, true) && before.count == 2 && before.cursor == 0 &&
			before.alt_cursor == 0 && before.map_file == "TDM_A.BMS");
}

// MissionList_InsertEntryAtIndex: the row check one past the catalog, the tail moved up, the
// cursors stepped strictly past the index (an insert AT the cursor takes it), no launch-option
// clear, the regrow by five keeping the alt cursor, the position clamped (D-NET-365).
void test_insert() {
	const auto cat = catalog();
	MissionRotation list = seeded(cat, {0, 2, 3}); // capacity 3, full
	CHECK(list.cursor == 0 && list.alt_cursor == -1);
	CHECK(list.set_alt_cursor(2));
	list.cursor = 1;
	// Before both cursors: both step up; the regrow keeps the alt cursor.
	list.launch_option(cat, 2) = 1; // a DM row: insert leaves it, append would clear it
	list.insert_entry(cat, 2, 0, 1);
	CHECK(list.capacity() == 8 && list.count == 4);
	CHECK(list.entry(0).catalog_index == 2 && list.entry(0).flag == 1);
	CHECK(list.entry(1).catalog_index == 0 && list.entry(2).catalog_index == 2 &&
			list.entry(3).catalog_index == 3);
	CHECK(list.cursor == 2 && list.alt_cursor == 3);
	CHECK(list.launch_options[2] == 1);
	// At the cursor's index: the cursor stays, so the new entry becomes the current one.
	list.insert_entry(cat, 1, 2, 0);
	CHECK(list.count == 5 && list.cursor == 2 && list.entry(2).catalog_index == 1 &&
			list.alt_cursor == 4);
	// Past the count: clamped to the append's slot; negative: clamped to the head.
	list.insert_entry(cat, 3, 99, 0);
	CHECK(list.count == 6 && list.entry(5).catalog_index == 3 && list.cursor == 2);
	list.insert_entry(cat, 0, -7, 0);
	CHECK(list.count == 7 && list.entry(0).catalog_index == 0 && list.cursor == 3 &&
			list.alt_cursor == 5);
	// The row check: one past the catalog is accepted, past that nothing.
	list.insert_entry(cat, static_cast<int32_t>(cat.size()) + 1, 0, 0);
	list.insert_entry(cat, -1, 0, 0);
	CHECK(list.count == 7);
	list.insert_entry(cat, static_cast<int32_t>(cat.size()), 7, 0);
	CHECK(list.count == 8 && list.entry(7).catalog_index == static_cast<int32_t>(cat.size()));
	// A null list allocates lazily.
	MissionRotation lazy;
	lazy.insert_entry(cat, 2, 5, 0);
	CHECK(lazy.allocated && lazy.count == 1 && lazy.entry(0).catalog_index == 2 && lazy.cursor == -1);
}

// After the in-game MISSION CLEAR and a later ADD the cursor is -1: the advance starts at entry
// 0 whatever the header word before the entries holds (D-NET-366; retail's fresh-block 0 does
// the same, any other word sends its one-shot removal over the header).
void test_cursor_after_clear() {
	const auto cat = catalog();
	MissionRotation list = seeded(cat, {0, 2});
	list.free_buffer();
	list.append(cat, 3, 0); // the lazy alloc: cursor -1, alt 0
	list.append(cat, 2, 0); // the regrow: alt -1, which retail would read as a one-shot flag
	CHECK(list.cursor == -1 && list.alt_cursor == -1);
	CHECK(list.advance(cat, false) && list.cursor == 0 && list.map_file == "CTF_D.BMS" &&
			list.count == 2);
	MissionRotation set_next = seeded(cat, {0});
	set_next.free_buffer();
	set_next.append(cat, 0, 0);
	set_next.append(cat, 2, 0);
	CHECK(set_next.set_alt_cursor(1));
	CHECK(set_next.advance(cat, false) && set_next.cursor == 0 && set_next.alt_cursor == -1);
}

// The admin console's seam over the host rotation (rotation_admin.h).
void test_host_rotation_admin() {
	const auto cat = catalog();
	HostRotation rotation;
	seed_rotation_from_host_screen(rotation, cat, {0, 2}, std::vector<int32_t>(cat.size(), 0));
	HostRotationAdmin admin(rotation, cat);
	RotationAdmin::List list = admin.list();
	CHECK(list.exists && list.entries.size() == 2 && list.cursor == 0 && list.alt_cursor == -1);
	CHECK(list.entries[1].catalog_index == 2 && !list.entries[1].one_shot && !list.half_flipped);
	const std::vector<RotationAdmin::CatalogRow> rows = admin.catalog();
	CHECK(rows.size() == cat.size() && rows[3].file == "CTF_D.BMS" && rows[0].launch_option == 0);
	admin.set_launch_option(3, 1);
	CHECK(admin.catalog()[3].launch_option == 1);
	// ADD with no position appends (the option cleared on a non-team row), with one inserts.
	admin.set_launch_option(2, 1);
	admin.add(2, std::nullopt, true);
	CHECK(rotation.list.count == 3 && rotation.list.entry(2).flag == 1 &&
			admin.catalog()[2].launch_option == 0);
	// An insert at the cursor's own index takes the cursor (the new entry is the current one).
	admin.add(3, 0, false);
	CHECK(rotation.list.count == 4 && rotation.list.entry(0).catalog_index == 3 &&
			rotation.list.cursor == 0);
	list = admin.list();
	CHECK(list.entries[3].one_shot && list.cursor == 0);
	// SETNEXT: the latch is set even when the index is refused.
	CHECK(!admin.set_next(9) && rotation.setnext_latch);
	rotation.setnext_latch = false;
	CHECK(admin.set_next(3) && rotation.setnext_latch && rotation.list.alt_cursor == 3);
	CHECK(admin.remove(0) && !admin.remove(9) && rotation.list.count == 3 &&
			rotation.list.alt_cursor == 2);
	// CLEAR outside the Game Loop has no host-screen marks to clear; inside it frees the list.
	admin.clear(false);
	CHECK(rotation.list.allocated && rotation.list.count == 3);
	admin.clear(true);
	CHECK(!rotation.list.allocated && !admin.list().exists);
	rotation.is_flipped = true;
	CHECK(admin.list().half_flipped);
}

void test_host_screen_seed() {
	const auto cat = catalog();
	HostRotation r;
	// START allocates at the table's row count and appends every row with flag
	// 0; the first row is the starting map, its mode the previous-mode word.
	std::vector<int32_t> switches(cat.size(), 0);
	switches[0] = 1; // the Switch cell set on the TDM row
	switches[2] = 1; // a DM row cannot keep it
	seed_rotation_from_host_screen(r, cat, {2, 0, 2}, switches);
	CHECK(r.list.capacity() == 3 && r.list.count == 3);
	CHECK(r.list.entry(0).flag == 0 && r.list.entry(1).flag == 0 && r.list.entry(2).flag == 0);
	CHECK(r.list.cursor == 0 && r.list.alt_cursor == -1);
	CHECK(r.list.map_file == "DM_C.BMS" && r.list.map_launch_option == 0);
	CHECK(r.list.launch_options[0] == 1 && r.list.launch_options[2] == 0);
	CHECK(r.previous_game_type == game_type::kDeathmatch);
	// The host screen's ADD default and its Switch eligibility.
	CHECK(game_type::host_rotation_default(game_type::kTeamDeathmatch));
	CHECK(!game_type::host_rotation_default(game_type::kObjectiveCoop));
	CHECK(!game_type::host_rotation_default(game_type::kDeathmatch));
}

// The round end's linger expiry on the authority: exit 4 under REPLAY with
// LASTGAME off, else 3; the session stays up. LASTGAME clears only at the
// session's create and destroy (Server_InitNewRoundState).
void test_round_end_exit_reason() {
	struct Case {
		uint32_t replay;
		bool last_game;
		int32_t reason;
	};
	for (const Case &c : {Case{1, false, kMissionExitRoundOver}, Case{1, true, kMissionExitMapCycle},
				 Case{0, false, kMissionExitMapCycle}, Case{0, true, kMissionExitMapCycle}}) {
		world::World world;
		world.registry.configure_pool(0, 8);
		world.rules.mp_session = true;
		HostRotation rotation;
		rotation.last_game = c.last_game;
		NapiNPServerCtx ctx;
		ctx.world = &world;
		ctx.rotation = &rotation;
		ctx.is_authority = 1;
		ctx.is_in_session = 1;
		ctx.config.replay_enabled = c.replay;
		ctx.round_end_announced = true;
		ctx.round_end_linger_ticks = 2;
		Server_TickUpdate(ctx);
		CHECK(ctx.mission_exit_reason == 0 && ctx.round_end_linger_ticks == 1);
		Server_TickUpdate(ctx);
		CHECK(ctx.mission_exit_reason == c.reason);
		CHECK(ctx.is_in_session == 1);
		// The toggle survives the round; the session init clears it.
		CHECK(rotation.last_game == c.last_game);
		Server_InitNewRoundState(ctx);
		CHECK(!rotation.last_game);
	}
}

// A host role in a session with two kept slots, team 1 and team 2.
struct RouterRig {
	std::vector<mission_catalog::Row> cat = catalog();
	HostRotation rotation;
	HostRole role{RoleKind::DedicatedHost};

	explicit RouterRig(const std::vector<int32_t> &rows, std::vector<int32_t> switches) {
		seed_rotation_from_host_screen(rotation, cat, rows, switches);
		role.set_rotation(&rotation);
		NapiNPServerCtx &ctx = role.state.host_owner.ctx;
		ctx.rotation = &rotation;
		ctx.is_authority = 1;
		ctx.is_in_session = 1;
		for (uint8_t team : {uint8_t{1}, uint8_t{2}}) {
			NapiNPConnection conn;
			conn.phase = ConnectionPhase::Spawned;
			conn.connection_id = 10u + team;
			conn.assigned_team_valid = true;
			conn.assigned_team = team;
			conn.reply.player_slot = team;
			ctx.np_protocol.connection_list.push_back(conn);
		}
	}
	// One round's end: every slot holds a live entity, then the map change.
	MapChangeStep round_end() {
		uint16_t slot = 3;
		for (NapiNPConnection &conn : role.state.host_owner.ctx.np_protocol.connection_list)
			conn.link.owned_entity = world::EntityHandle::make(0, slot++);
		return begin_host_map_change(role, cat);
	}
	uint8_t team(size_t i) const {
		return role.state.host_owner.ctx.np_protocol.connection_list[i].assigned_team;
	}
};

void test_router_halves_and_latch() {
	std::vector<int32_t> switches(4, 0);
	switches[0] = 1; // TDM_A plays as two halves

	// A launch-option map: its first half's end replays the same entry with
	// the sides swapped; its second half's end swaps back and advances.
	{
		RouterRig rig({0, 2}, switches);
		CHECK(rig.rotation.list.map_launch_option == 1 && !rig.rotation.is_flipped);
		CHECK(rig.round_end() == MapChangeStep::NextMission);
		CHECK(rig.rotation.is_flipped && rig.rotation.list.cursor == 0);
		CHECK(rig.rotation.list.map_file == "TDM_A.BMS");
		CHECK(rig.team(0) == 2 && rig.team(1) == 1);
		CHECK(rig.rotation.side_team[0] == 2 && rig.rotation.side_team[1] == 1);
		// The slot reset kept the slots and dropped their entities.
		CHECK(!rig.role.state.host_owner.ctx.np_protocol.connection_list[0].link.owned_entity.valid());
		CHECK(rig.role.state.host_owner.ctx.np_protocol.connection_list[0].phase ==
				ConnectionPhase::PlayerAdded);
		CHECK(rig.round_end() == MapChangeStep::NextMission);
		CHECK(!rig.rotation.is_flipped && rig.rotation.list.cursor == 1);
		CHECK(rig.rotation.list.map_file == "DM_C.BMS");
		CHECK(rig.team(0) == 1 && rig.team(1) == 2);
		CHECK(rig.rotation.side_team[0] == 1 && rig.rotation.side_team[1] == 2);
		// No option on the DM map: the advance runs out with REPLAY off.
		CHECK(rig.round_end() == MapChangeStep::RotationEnded);
		CHECK(rig.team(0) == 1 && rig.team(1) == 2);
	}
	// The last entry's second half still plays with REPLAY off: the replay
	// never calls the advance.
	{
		RouterRig rig({2, 0}, switches);
		CHECK(rig.round_end() == MapChangeStep::NextMission); // DM_C -> TDM_A
		CHECK(rig.rotation.list.cursor == 1 && !rig.rotation.is_flipped);
		CHECK(rig.round_end() == MapChangeStep::NextMission); // TDM_A's second half
		CHECK(rig.rotation.is_flipped && rig.rotation.list.cursor == 1);
		CHECK(rig.round_end() == MapChangeStep::RotationEnded);
		CHECK(!rig.rotation.is_flipped);
	}
	// A SETNEXT at a first half's end: no swap, the toggle is set and the
	// latch cleared, so the router clears the toggle and advances onto the
	// SETNEXT entry: the second half is skipped and the sides stay.
	{
		RouterRig rig({0, 2, 3}, switches);
		CHECK(rig.rotation.list.set_alt_cursor(2));
		rig.rotation.setnext_latch = true;
		CHECK(rig.round_end() == MapChangeStep::NextMission);
		CHECK(!rig.rotation.setnext_latch && !rig.rotation.is_flipped);
		CHECK(rig.rotation.list.cursor == 2 && rig.rotation.list.map_file == "CTF_D.BMS");
		CHECK(rig.team(0) == 1 && rig.team(1) == 2);
		CHECK(rig.rotation.side_team[0] == 1);
	}
	// A SETNEXT during a second half: the swap restores the sides, the latch
	// sets the toggle and clears, and the router advances onto the entry.
	{
		RouterRig rig({0, 2, 3}, switches);
		CHECK(rig.round_end() == MapChangeStep::NextMission); // the second half
		CHECK(rig.team(0) == 2);
		CHECK(rig.rotation.list.set_alt_cursor(2));
		rig.rotation.setnext_latch = true;
		CHECK(rig.round_end() == MapChangeStep::NextMission);
		CHECK(!rig.rotation.setnext_latch && !rig.rotation.is_flipped);
		CHECK(rig.rotation.list.cursor == 2 && rig.team(0) == 1 && rig.team(1) == 2);
		CHECK(rig.rotation.side_team[0] == 1);
	}
	// A SETNEXT on a map without the option leaves the latch set (the swap
	// arm needs the option), so the next launch-option map loses its second
	// half (a retail quirk, §5.70.5).
	{
		RouterRig rig({2, 0, 3}, switches);
		CHECK(rig.rotation.list.set_alt_cursor(1));
		rig.rotation.setnext_latch = true;
		CHECK(rig.round_end() == MapChangeStep::NextMission); // DM_C -> TDM_A
		CHECK(rig.rotation.setnext_latch && rig.rotation.list.cursor == 1);
		CHECK(rig.round_end() == MapChangeStep::NextMission); // TDM_A's first half ends
		CHECK(rig.rotation.list.cursor == 2 && !rig.rotation.is_flipped);
		CHECK(!rig.rotation.setnext_latch && rig.team(0) == 1);
	}
	// Out of a session the map change has nowhere to go.
	{
		RouterRig rig({0, 2}, switches);
		rig.role.state.host_owner.ctx.is_in_session = 0;
		CHECK(rig.round_end() == MapChangeStep::RotationEnded);
	}
}

// The team assignment's side arms answer through the side-to-team map.
void test_side_map_team_assignment() {
	world::World world;
	world.registry.configure_pool(0, 8);
	GameConfig config;
	config.game_type = game_type::kTeamDeathmatch;
	config.num_teams = 2;
	config.side_a_password = "blue";
	HostRotation rotation;
	std::vector<NapiNPConnection> roster(1);
	roster[0].join_password = "blue";
	CHECK(Server_ReservePlayerTeam(config, true, roster, roster[0], world, &rotation) == 1);
	rotation.side_team = {2, 1};
	std::vector<NapiNPConnection> swapped(1);
	swapped[0].join_password = "blue";
	CHECK(Server_ReservePlayerTeam(config, true, swapped, swapped[0], world, &rotation) == 2);
}

} // namespace

int main() {
	test_alloc_append_find();
	test_remove_alt_reset_free();
	test_advance();
	test_one_shot();
	test_insert();
	test_cursor_after_clear();
	test_host_rotation_admin();
	test_host_screen_seed();
	test_round_end_exit_reason();
	test_router_halves_and_latch();
	test_side_map_team_assignment();
	if (failures != 0) {
		std::printf("mission_rotation: %d failure(s)\n", failures);
		return 1;
	}
	std::printf("mission_rotation: ok\n");
	return 0;
}
