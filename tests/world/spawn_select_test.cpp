// Player spawn-point selection (net-re §5.2c/§5.61): one operation owns the
// picked-zone pose and the exact no-pick per-mode marker chains.
// [orig: Server_PositionPlayerForSpawn @0x50cf60 -> Entity_FindBestSpawnPoint @0x50ccc0]
#include "world/entity.h"
#include "world/spawn_select.h"
#include "world/world.h"

#include <cmath>
#include <cstdio>

using namespace opennova::world;

static int failures = 0;
#define CHECK(c) \
    do { if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

namespace {

EntityHandle spawn_marker(World &w, int32_t item_id, Vec3 pos, int16_t yaw = 0) {
    Entity e;
    e.kind = EntityKind::Marker; // markers promote into pool 3
    e.item_id = item_id;
    e.has_item_def = true;
    e.position = pos;
    e.yaw = yaw;
    return w.registry.spawn(3, e);
}

EntityHandle spawn_body(World &w, Vec3 pos, bool player, bool alive = true,
                        uint8_t team = 0) {
    Entity e;
    e.kind = EntityKind::Organic;
    e.item_id = 0x14B9;
    e.position = pos;
    e.alive = alive;
    e.team = team;
    if (player) {
        e.flags |= kEntityFlagPlayer;
        e.engine_flags |= kEntityFlagPlayer;
    }
    return w.registry.spawn(0, e);
}

EntityHandle spawn_zone(World &w, int pool, uint8_t team, uint8_t number,
                        int32_t control = 0x10000) {
    Entity e;
    e.kind = EntityKind::Item;
    e.is_spawn_point = true;
    e.team = team;
    e.zone_number = number;
    e.zone_control = control;
    e.alive = true;
    return w.registry.spawn(pool, e);
}

bool approx(float a, float b) { return std::fabs(a - b) < 1e-3f; }

void credit_team_capture(World &w, EntityHandle player) {
    MatchRules rules;
    rules.game_type = 0x10004u; // CTF; the event writes CRenderState field 6.
    w.match.configure(rules);
    MatchPlayerIdentity identity;
    identity.entity = player;
    w.match.upsert_player(identity);
    w.match.record_flag_capture(w, player);
}

} // namespace

int main() {
    // --- Non-team primary 6095 wins over fallback 6002. Best-point scoring
    //     considers every pool-0 Flags&0x100 row (even dead), ignores NPCs,
    //     and excludes the player being positioned.
    // [orig: Server_PositionPlayerForSpawn @0x50D213;
    // Entity_FindBestSpawnPoint @0x50CCC0]
    {
        World w;
        w.registry.configure_pool(0, 16);
        w.registry.configure_pool(3, 16);

        const EntityHandle self = spawn_body(w, {500.0f, 0.0f, 0.0f}, true);
        spawn_body(w, {0.0f, 0.0f, 0.0f}, true, false); // dead player still avoids
        spawn_body(w, {200.0f, 10.0f, 3.0f}, false);    // NPC at winner is ignored
        spawn_marker(w, 6095, {2.0f, 0.0f, 0.0f}, 30);
        spawn_marker(w, 6095, {200.0f, 10.0f, 3.0f}, 90);
        spawn_marker(w, 6002, {900.0f, 0.0f, 0.0f}, 10);

        const SpawnPointResult r = resolve_player_spawn_pose(
            w, self, EntityHandle{}, 0, 0, 0x00000u);
        CHECK(r.found);
        CHECK(approx(r.position.x, 200.0f));
        CHECK(approx(r.position.y, 10.0f));
        CHECK(approx(r.position.z, 3.0f));
        CHECK(r.yaw == 90);
        CHECK(w.spawn_cycle_counter == 1);
    }

    // --- Excluding the body being positioned leaves no avoid rows, so the
    //     first fallback marker wins. Whole-unit distance ties also retain
    //     pool order under retail's descending shell sort.
    {
        World w;
        w.registry.configure_pool(0, 16);
        w.registry.configure_pool(3, 16);
        const EntityHandle self = spawn_body(w, {0.0f, 0.0f, 0.0f}, true);
        spawn_body(w, {10.0f, 0.0f, 0.0f}, true);
        spawn_marker(w, 6002, {0.25f, 0.0f, 0.0f}, 11);
        spawn_marker(w, 6002, {0.75f, 0.0f, 0.0f}, 22);
        const SpawnPointResult r = resolve_player_spawn_pose(
            w, self, EntityHandle{}, 0, 0, 0x00000u);
        CHECK(r.found);
        CHECK(approx(r.position.x, 0.25f));
        CHECK(r.yaw == 11);
        CHECK(w.spawn_cycle_counter == 1);
    }

    // --- Stock/training Co-op is the exact 00TRa route: no mission-mode bit
    //     maps to game type 0x10020, whose 6094 primary falls back to 6001.
    //     Direct Co-op selection uses player-slot modulo count, not distance.
    {
        World w;
        w.registry.configure_pool(0, 16);
        w.registry.configure_pool(3, 16);
        spawn_body(w, {297.81f, -409.12f, 27.14f}, true);
        spawn_marker(w, 6005, {300.0f, -377.0f, 27.0f});
        spawn_marker(w, 6001, {10.0f, 10.0f, 0.0f}, 11);
        spawn_marker(w, 6001, {297.81f, -409.12f, 27.14f}, 22);
        const SpawnPointResult r = resolve_player_spawn_pose(
            w, EntityHandle{}, EntityHandle{}, 1, 1, 0x10020u);
        CHECK(r.found);
        CHECK(approx(r.position.x, 297.81f));
        CHECK(r.yaw == 22);
        CHECK(w.spawn_cycle_counter == 0);
    }

    // --- A parented Co-op primary keeps its full local pose, then applies
    //     Entity_TransformLocalToWorld. Pitch/roll remain marker-local.
    {
        World w;
        w.registry.configure_pool(1, 4);
        w.registry.configure_pool(3, 16);
        Entity parent;
        parent.kind = EntityKind::Item;
        parent.position = {100.0f, 50.0f, 10.0f};
        parent.yaw = 0;
        const EntityHandle parent_h = w.registry.spawn(1, parent);
        const EntityHandle marker_h = spawn_marker(w, 6094, {1.0f, 0.0f, 2.0f}, 90);
        Entity *marker = w.registry.get(marker_h);
        marker->pitch = 4;
        marker->roll = 5;
        marker->ground_target = parent_h;
        const SpawnPointResult r = resolve_player_spawn_pose(
            w, EntityHandle{}, EntityHandle{}, 0, 1, 0x30020u);
        CHECK(r.found);
        CHECK(approx(r.position.x, 100.0f));
        CHECK(approx(r.position.y, 51.0f));
        CHECK(approx(r.position.z, 12.0f));
        CHECK(r.yaw == 0 && r.pitch == 4 && r.roll == 5);
    }

    // --- CRenderState field 6 (raw FlagCaptures) suppresses the primary
    //     marker in solo, team and Co-op families, leaving the fallback.
    // [orig: CRenderState_GetFieldByIndex(..., 6) @0x50D1DB]
    {
        World w;
        w.registry.configure_pool(0, 16);
        w.registry.configure_pool(3, 16);
        const EntityHandle scorer = spawn_body(w, {}, true, true, 1);
        credit_team_capture(w, scorer);
        spawn_marker(w, 6096, {-100.0f, 0.0f, 0.0f});
        spawn_marker(w, 6003, {100.0f, 0.0f, 0.0f});
        SpawnPointResult r = resolve_player_spawn_pose(
            w, EntityHandle{}, EntityHandle{}, 0, 1, 0x10010u);
        CHECK(r.found && approx(r.position.x, 100.0f));

        World coop;
        coop.registry.configure_pool(0, 4);
        coop.registry.configure_pool(3, 4);
        const EntityHandle coop_scorer = spawn_body(coop, {}, true, true, 1);
        credit_team_capture(coop, coop_scorer);
        spawn_marker(coop, 6094, {-50.0f, 0.0f, 0.0f});
        spawn_marker(coop, 6001, {50.0f, 0.0f, 0.0f});
        r = resolve_player_spawn_pose(
            coop, EntityHandle{}, EntityHandle{}, 0, 1, 0x10020u);
        CHECK(r.found && approx(r.position.x, 50.0f));

        World solo;
        solo.registry.configure_pool(0, 4);
        solo.registry.configure_pool(3, 4);
        const EntityHandle solo_scorer = spawn_body(solo, {}, true, true, 0);
        credit_team_capture(solo, solo_scorer);
        spawn_marker(solo, 6095, {-25.0f, 0.0f, 0.0f});
        spawn_marker(solo, 6002, {25.0f, 0.0f, 0.0f});
        r = resolve_player_spawn_pose(
            solo, EntityHandle{}, EntityHandle{}, 0, 0, 0x00000u);
        CHECK(r.found && approx(r.position.x, 25.0f));
    }

    // --- Every known retail code word reaches one of the three witnessed
    //     selector families; this guards future mode additions from acquiring
    //     a fourth, accidental dispatch policy.
    {
        World w;
        w.registry.configure_pool(3, 8);
        spawn_marker(w, 6095, {10.0f, 0.0f, 0.0f});
        spawn_marker(w, 6097, {20.0f, 0.0f, 0.0f});
        spawn_marker(w, 6094, {30.0f, 0.0f, 0.0f});
        const uint32_t solo_modes[] = {0x00000u, 0x00001u, 0x00008u};
        for (const uint32_t mode : solo_modes) {
            const SpawnPointResult r = resolve_player_spawn_pose(
                w, EntityHandle{}, EntityHandle{}, 0, 0, mode);
            CHECK(r.found && approx(r.position.x, 10.0f));
        }
        const uint32_t team_modes[] = {
            0x10000u, 0x10001u, 0x10002u, 0x10004u, 0x10008u,
            0x10010u, 0x50010u, 0x90002u,
        };
        for (const uint32_t mode : team_modes) {
            const SpawnPointResult r = resolve_player_spawn_pose(
                w, EntityHandle{}, EntityHandle{}, 0, 2, mode);
            CHECK(r.found && approx(r.position.x, 20.0f));
        }
        for (const uint32_t mode : {0x10020u, 0x30020u}) {
            const SpawnPointResult r = resolve_player_spawn_pose(
                w, EntityHandle{}, EntityHandle{}, 0, 1, mode);
            CHECK(r.found && approx(r.position.x, 30.0f));
        }
    }

    // --- Objective Co-op's last fallback is the first pool-1, then pool-2,
    //     numbered team spawn entity. It gets the named-userpoint fallback
    //     lift but does not enter the picked-zone 6007 scatter arm.
    {
        World w;
        w.registry.configure_pool(1, 4);
        w.registry.configure_pool(2, 4);
        w.registry.configure_pool(3, 4);
        const EntityHandle p2 = spawn_zone(w, 2, 1, 3);
        const EntityHandle p1 = spawn_zone(w, 1, 1, 2);
        w.registry.get(p2)->position = {200.0f, 0.0f, 2.0f};
        w.registry.get(p1)->position = {100.0f, 0.0f, 3.0f};
        w.registry.get(p1)->has_item_def = true;
        w.registry.get(p2)->has_item_def = true;
        spawn_marker(w, 6007, {100.0f, 0.0f, 20.0f});
        const SpawnPointResult r = resolve_player_spawn_pose(
            w, EntityHandle{}, EntityHandle{}, 0, 1, 0x30020u);
        CHECK(r.found && approx(r.position.x, 100.0f));
        CHECK(approx(r.position.z, 4.0f));
        CHECK(w.spawn_cycle_counter == 0);
    }

    // --- No cross-family safety net: a DM does not consume Co-op/team
    //     markers. The failed non-Co-op fallback still advances the shared
    //     cycle exactly once.
    {
        World w;
        w.registry.configure_pool(3, 8);
        spawn_marker(w, 6001, {1.0f, 1.0f, 0.0f});
        spawn_marker(w, 6096, {2.0f, 2.0f, 0.0f});
        const SpawnPointResult r = resolve_player_spawn_pose(
            w, EntityHandle{}, EntityHandle{}, 0, 0, 0x00000u);
        CHECK(!r.found);
        CHECK(w.spawn_cycle_counter == 1);
    }

    // --- A numbered deploy target cycles over the target itself plus the first
    //     32 in-radius pool-3 type-6007 markers. Counter 0 retains the target's
    //     userpoint fallback (+1 z); marker choices replace the complete pose
    //     and do not inherit that lift.
    // [orig: Server_PositionPlayerForSpawn @0x50CF60, scatter arm
    //  @0x50D04D..0x50D18D]
    {
        World w;
        w.registry.configure_pool(2, 4);
        w.registry.configure_pool(3, 8);
        const EntityHandle zone = spawn_zone(w, 2, 1, 1);
        Entity *target = w.registry.get(zone);
        target->position = {10.0f, 20.0f, 3.0f};
        target->yaw = 30;
        target->zone_radius = 5;
        spawn_marker(w, 6007, {12.0f, 20.0f, 7.0f}, 60);
        spawn_marker(w, 6007, {100.0f, 20.0f, 9.0f}, 90); // outside

        SpawnPointResult pose = resolve_player_spawn_pose(
            w, EntityHandle{}, zone, 0, 1, 0x10010u);
        CHECK(pose.found && approx(pose.position.x, 10.0f));
        CHECK(approx(pose.position.z, 4.0f));
        CHECK(pose.yaw == 30);
        CHECK(w.spawn_cycle_counter == 1);

        pose = resolve_player_spawn_pose(
            w, EntityHandle{}, zone, 0, 1, 0x10010u);
        CHECK(approx(pose.position.x, 12.0f));
        CHECK(approx(pose.position.z, 7.0f));
        CHECK(pose.yaw == 60);
        CHECK(w.spawn_cycle_counter == 2);

        pose = resolve_player_spawn_pose(
            w, EntityHandle{}, zone, 0, 1, 0x10010u);
        CHECK(approx(pose.position.x, 10.0f));
        CHECK(approx(pose.position.z, 4.0f));
        CHECK(w.spawn_cycle_counter == 3);
    }

    // --- A parented 6007 keeps local coordinates until selected, then takes
    //     Entity_TransformLocalToWorld's full parent pose. The transform adds
    //     headings while leaving the marker pitch/roll local.
    // [orig: Server_PositionPlayerForSpawn @0x50D155;
    // Entity_TransformLocalToWorld @0x43BD00]
    {
        World w;
        w.registry.configure_pool(1, 4);
        w.registry.configure_pool(2, 4);
        w.registry.configure_pool(3, 4);
        const EntityHandle zone = spawn_zone(w, 2, 1, 2);
        Entity *target = w.registry.get(zone);
        target->position = {0.0f, 0.0f, 0.0f};
        target->zone_radius = 5;

        Entity parent;
        parent.kind = EntityKind::Item;
        parent.position = {100.0f, 50.0f, 10.0f};
        parent.yaw = 0; // heading 90 degrees: local +X becomes world +Y
        const EntityHandle parent_handle = w.registry.spawn(1, parent);
        const EntityHandle marker = spawn_marker(w, 6007, {1.0f, 0.0f, 2.0f}, 90);
        w.registry.get(marker)->pitch = 4;
        w.registry.get(marker)->roll = 5;
        w.registry.get(marker)->ground_target = parent_handle;

        (void)resolve_player_spawn_pose(
            w, EntityHandle{}, zone, 0, 1, 0x10010u); // cycle 0 = target
        const SpawnPointResult pose = resolve_player_spawn_pose(
            w, EntityHandle{}, zone, 0, 1, 0x10010u);
        CHECK(approx(pose.position.x, 100.0f));
        CHECK(approx(pose.position.y, 51.0f));
        CHECK(approx(pose.position.z, 12.0f));
        CHECK(pose.yaw == 0);
        CHECK(pose.pitch == 4);
        CHECK(pose.roll == 5);
    }

    // With no in-radius 6007 candidate the retail counter is untouched.
    {
        World w;
        w.registry.configure_pool(2, 4);
        w.registry.configure_pool(3, 4);
        const EntityHandle zone = spawn_zone(w, 2, 1, 3);
        Entity *target = w.registry.get(zone);
        target->position = {0.0f, 0.0f, 0.0f};
        target->zone_radius = 2;
        spawn_marker(w, 6007, {10.0f, 0.0f, 0.0f});
        w.spawn_cycle_counter = 17;
        const SpawnPointResult pose = resolve_player_spawn_pose(
            w, EntityHandle{}, zone, 0, 1, 0x10010u);
        CHECK(approx(pose.position.z, 1.0f));
        CHECK(w.spawn_cycle_counter == 17);
    }

	// --- SpawnZoneList membership, and the target-less respawn restriction's
	//     exact team-zone predicate. The original helper does not count living
	//     players (despite its provisional reverse-engineered name): an
	//     unnumbered same-team zone always qualifies, while a numbered one must
	//     have full control. Registry membership itself has no alive filter.
	// [orig: Entity_BuildSpawnZoneList @0x43EAE0;
	//  Entity_HasAliveEntityOfTeam @0x4FC7B0]
	{
		World w;
		w.registry.configure_pool(2, 4);
		const EntityHandle base = spawn_zone(w, 2, 1, 0);
		w.registry.get(base)->alive = false;
		const EntityHandle numbered = spawn_zone(w, 2, 2, 1, 0xFFFF);

		CHECK(world_has_spawn_zone(w));
		CHECK(team_has_available_spawn_zone(w, 1));
		CHECK(!team_has_available_spawn_zone(w, 2));
		w.registry.get(numbered)->zone_control = 0x10000;
		CHECK(team_has_available_spawn_zone(w, 2));
		CHECK(!team_has_available_spawn_zone(w, 3));
	}

	// --- SpawnWaveList_BuildFromMission: retail defaults put NUMBERED zones on
	//     the 10-second list while an unnumbered base remains immediate. A zero
	//     numbered-zone option falls back to the nonzero base option.
	// [orig: Config_SetDefaults @0x54D030; SpawnWaveList_BuildFromMission @0x52A920]
	{
		World w;
		w.registry.configure_pool(1, 16);
		w.registry.configure_pool(2, 16);
		const EntityHandle numbered = spawn_zone(w, 2, 1, 3);
		const EntityHandle base = spawn_zone(w, 1, 1, 0);
		w.spawn_waves.build_from_mission(w, 0, 10);
		CHECK(w.spawn_waves.entries().size() == 1);
		CHECK(w.spawn_waves.has_entry(numbered));
		CHECK(!w.spawn_waves.has_entry(base));
		CHECK(w.spawn_waves.entries()[0].interval == 10);

		w.spawn_waves.build_from_mission(w, 7, 0);
		CHECK(w.spawn_waves.entries().size() == 2);
		CHECK(w.spawn_waves.entries()[0].interval == 7);
		CHECK(w.spawn_waves.entries()[1].interval == 7);
	}

	// --- Queue semantics: same-player duplicates are rejected, moving to a new
	//     group evicts the old row, team mismatch is rejected, and a group caps
	//     at the retail eight player pointers. Status ETA is position-sensitive.
	// [orig: SpawnWaveList_TryQueuePlayer @0x52A490;
	//  SpawnWaveList_GetEntryInfo @0x52A700]
	{
		World w;
		w.registry.configure_pool(0, 16);
		w.registry.configure_pool(2, 16);
		const EntityHandle a = spawn_zone(w, 2, 1, 1);
		const EntityHandle b = spawn_zone(w, 2, 1, 2);
		w.spawn_waves.build_from_mission(w, 0, 10);
		std::vector<EntityHandle> players;
		for (int i = 0; i < 9; ++i) {
			Entity p;
			p.kind = EntityKind::Organic;
			p.team = (i == 8) ? 2 : 1;
			players.push_back(w.registry.spawn(0, p));
		}
		CHECK(w.spawn_waves.try_queue(w, a, players[0]));
		CHECK(!w.spawn_waves.try_queue(w, a, players[0]));
		CHECK(w.spawn_waves.try_queue(w, b, players[0]));
		CHECK(w.spawn_waves.entries()[0].queued.empty());
		CHECK(w.spawn_waves.entries()[1].queued.size() == 1);
		CHECK(!w.spawn_waves.try_queue(w, b, players[8]));
		for (int i = 1; i < 8; ++i)
			CHECK(w.spawn_waves.try_queue(w, b, players[i]));
		Entity extra;
		extra.kind = EntityKind::Organic;
		extra.team = 1;
		const EntityHandle ninth = w.registry.spawn(0, extra);
		CHECK(!w.spawn_waves.try_queue(w, b, ninth));
		CHECK(w.spawn_waves.entries()[1].queued.size() == 8);
		CHECK(w.spawn_waves.entries()[1].requester_countdown(players[0]) == 0);
		CHECK(w.spawn_waves.entries()[1].requester_countdown(players[3]) == 30);
		CHECK(w.spawn_waves.entries()[1].requester_countdown(ninth) == 80);
	}

	// --- Tick semantics: countdown zero releases the head on the next 1 Hz
	//     pass, then exactly one member per interval. Losing full control while
	//     counting down flushes the whole group and both timers. A team flip does
	//     the same and refreshes the cached team.
	// [orig: SpawnWaveList_TickEntry @0x52A330;
	//  SpawnWaveList_ResetOnZoneTeamChange @0x52A5B0]
	{
		World w;
		w.registry.configure_pool(0, 8);
		w.registry.configure_pool(2, 8);
		const EntityHandle zone = spawn_zone(w, 2, 1, 1);
		Entity p1;
		p1.kind = EntityKind::Organic;
		p1.team = 1;
		Entity p2 = p1;
		const EntityHandle h1 = w.registry.spawn(0, p1);
		const EntityHandle h2 = w.registry.spawn(0, p2);
		w.spawn_waves.build_from_mission(w, 0, 2);
		CHECK(w.spawn_waves.try_queue(w, zone, h1));
		CHECK(w.spawn_waves.try_queue(w, zone, h2));
		auto released = w.spawn_waves.tick(w);
		CHECK(released.size() == 1 && released[0].player == h1 &&
		      released[0].zone == zone);
		CHECK(w.spawn_waves.entries()[0].countdown == 2);
		CHECK(w.spawn_waves.tick(w).empty());
		CHECK(w.spawn_waves.entries()[0].countdown == 1);
		CHECK(w.spawn_waves.tick(w).empty());
		CHECK(w.spawn_waves.entries()[0].countdown == 0);
		released = w.spawn_waves.tick(w);
		CHECK(released.size() == 1 && released[0].player == h2);

		w.spawn_waves.build_from_mission(w, 0, 2);
		CHECK(w.spawn_waves.try_queue(w, zone, h1));
		CHECK(w.spawn_waves.tick(w).size() == 1);
		CHECK(w.spawn_waves.try_queue(w, zone, h2));
		w.registry.get(zone)->zone_control = 0xFFFF;
		CHECK(w.spawn_waves.tick(w).empty());
		CHECK(w.spawn_waves.entries()[0].queued.empty());
		CHECK(w.spawn_waves.entries()[0].countdown == 0);

		w.registry.get(zone)->zone_control = 0x10000;
		CHECK(w.spawn_waves.try_queue(w, zone, h1));
		w.registry.get(zone)->team = 2;
		w.spawn_waves.reset_on_zone_team_change(w, zone);
		CHECK(w.spawn_waves.entries()[0].queued.empty());
		CHECK(w.spawn_waves.entries()[0].team == 2);
	}

    if (failures == 0) std::printf("OK spawn_select\n");
    return failures == 0 ? 0 : 1;
}
