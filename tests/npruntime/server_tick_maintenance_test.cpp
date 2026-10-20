// Server_TickUpdate's per-tick and periodic-second maintenance legs ported from the
// jo-c cross-check (2026-09-12):
//   - the 620-tick spawn protection (entity+292): seeded at creation, join and every
//     deploy, counted down per authority tick in a live MP session, frozen during
//     pre-round and after round end, zeroed outside a session, held at -1 for a
//     spectator, and cleared by the first validated fire
//     [orig: Server_UpdateAllActivePlayerSlots @0x518820; Server_DeployPlayer
//      @0x517740; Server_ClientFiredRound @0x50c736..0x50c75d];
//   - the 1 Hz round-robin S2C 0x2F flag refresh [orig: Server_RefreshNextFlagState @0x517B20];
//   - the violation sweep's flag carry limit [orig: Server_CheckPlayerViolations
//     @0x51ABD0 -> Entity_DropCarriedObject @0x439DF0 + Entity_SyncPositionFromDefinition
//     @0x43A9B0] (the t6 punt arms are pinned in host_punt_test);
//   - the team-mode 1 Hz S2C 0x46 field-0x0008 downed resend [orig: Server_TickUpdate
//     @0x51E2D0..0x51E378];
//   - the death consumer's live entity+44 cause read and reported-bit clear
//     [orig: GameEvent_PlayerDeath @0x516DD0 — @0x516f4d, @0x517180..0x517206];
//   - the roster row's spectator latch mirror [orig: Server_PlayerAdd @0x51CD83;
//     Server_ConvertPlayerToSpectator @0x519E76];
//   - the armory-reuse cooldown and pre-round latch around C2S 0x2F
//     [orig: NapiNPServerMsg_HandlePlayerLoadout @0x515790];
//   - the 744-tick priority-target sweep [orig: EntityPool_ClearDirtyFlags @0x508E30];
//   - the 1 Hz / accept-time kit-weight recompute [orig: Server_RecalculateAllPlayerScores
//     @0x5014E0];
//   - the unconditional other-row eviction of SpawnWaveList_TryQueuePlayer @0x52A490;
//   - the C2S 0x0E Conquer & Control auto-pick rejection, the handle-0 admission and
//     the revive latch's targetless deploy [orig: Server_ProcessClientRequestRespawn @0x519AF0];
//   - the slot's damage-class bytes behind the S2C 0x5A fourth byte (D-NET-508).

#include <runtime/inmatch/napi_np_connection.h>
#include <runtime/inmatch/napi_np_protocol.h>
#include <runtime/inmatch/map_change.h>
#include <runtime/inmatch/napi_np_server_ctx.h>
#include <runtime/inmatch/server_loadout_grant.h>
#include <runtime/inmatch/server_message_dispatch.h>
#include <runtime/inmatch/server_initial_state.h> // Server_SendInitialGameStateToPlayer (the host's own join)
#include <runtime/inmatch/server_session.h>
#include <runtime/inmatch/server_player_death.h> // Server_ProcessPlayerDeath
#include <runtime/inmatch/server_spawn.h>
#include <runtime/inmatch/server_tick.h>
#include <runtime/inmatch/server_chat.h>            // the 0x32 join line (D-NET-593)
#include <runtime/inmatch/server_status_feed.h>     // the status page (D-NET-593)
#include <runtime/inmatch/server_visible_players.h> // the 0x4C snapshot (D-NET-593)
#include <runtime/hud/hud_server_status.h>
#include <net/npwire/visible_players.h>

#include <runtime/inmatch/loopback_channel.h>
#include <runtime/inmatch/session_transport.h>
#include <runtime/inmatch/udp_session_transport.h>
#include <runtime/replication/connection.h>

#include <net/npwire/ingame_decode.h>
#include <net/npwire/ingame_encode.h>
#include <net/npwire/ingame_message_id.h>
#include <net/npwire/protocol_message.h>

#include <base/gameprofile/game_type.h>
#include <runtime/world/ai.h>
#include <runtime/world/entity.h>
#include <runtime/world/local_player.h>
#include <runtime/world/player_spawn.h>
#include <runtime/world/spawn_select.h>
#include <runtime/world/weapon_inventory.h>
#include <runtime/world/world.h>

#include <algorithm>
#include <memory>
#include <array>
#include <cstdint>
#include <cstdio>
#include <map>
#include <initializer_list>
#include <utility>
#include <vector>

#include "conn_fixture.h"
#include "host_test_setup.h"

namespace {

using namespace opennova;
namespace inmatch = opennova::inmatch;
namespace ns = opennova::replication;
namespace w = opennova::world;

// The player entity's own weapon table (world::Entity::weapon_slots), the host's
// per-player slot rows since D-NET-336's move.
std::map<uint16_t, w::PersonalWeaponSlot> &slots_of(w::World &world,
		const inmatch::NapiNPConnection &conn) {
	return world.registry.get(conn.link.owned_entity)->weapon_slots;
}
using conn_fixture::make_seeded_conn;

bool expect(bool cond, const char *msg) {
	if (cond) return true;
	std::fprintf(stderr, "FAIL: %s\n", msg);
	return false;
}

w::PlayerSpawn player_spawn(float x, float y, float z, uint8_t team = 1) {
	w::PlayerSpawn s;
	s.position = {x, y, z};
	s.team = team;
	return s;
}

// Advance the host until the shared one-second service fires (inclusive).
void tick_to_periodic_second(inmatch::NapiNPServerCtx &ctx) {
	do {
		inmatch::Server_TickUpdate(ctx);
	} while (!ctx.world->match.periodic_second());
}

std::vector<ns::Datagram> drain(ns::UdpSessionTransport &transport) {
	std::vector<ns::Datagram> out;
	ns::Datagram datagram;
	while (transport.pop_outbound(datagram)) out.push_back(datagram);
	return out;
}

std::vector<ns::Datagram> drain(ns::LoopbackChannel &channel) {
	std::vector<ns::Datagram> out;
	ns::Datagram datagram;
	while (channel.client_recv(datagram)) out.push_back(datagram);
	return out;
}

std::vector<ObjectiveEntityState> objective_states(const std::vector<ns::Datagram> &datagrams) {
	std::vector<ObjectiveEntityState> out;
	for (const ns::Datagram &d : datagrams) {
		if (d.tag != s2c::OBJECTIVE_ENTITY_STATE) continue;
		ObjectiveEntityState state;
		size_t consumed = 0;
		if (decode_objective_entity_state(d.body.data(), d.body.size(), state, consumed) &&
				consumed == d.body.size() && d.body.size() == 19)
			out.push_back(state);
	}
	return out;
}

bool saw_flag_return_feed(const std::vector<ns::Datagram> &datagrams) {
	for (const ns::Datagram &d : datagrams) {
		if (d.tag == s2c::GAME_EVENT && d.body.size() == 8 &&
				d.body[0] >= 0x23 && d.body[0] <= 0x25)
			return true;
	}
	return false;
}

// A 45-B C2S 0x06 primary fire for `adm` at the next fresh client tick.
std::vector<uint8_t> fire_body(inmatch::NapiNPConnection &conn, uint16_t shooter,
		uint8_t adm, uint32_t &client_tick) {
	ClientFiredRound fire;
	fire.current_tick = ++client_tick;
	fire.shooter_handle = shooter;
	fire.target_handle = 0xFFFF;
	fire.adm_index = adm;
	fire.fire_flags = 0x22;
	(void)conn;
	return encode_client_fired_round(fire);
}

// The armory every kit-weight and fire case shares: adm 5 = a 30-round rifle at
// combo 197 (category 3, rank 2), one ammo class, 3 u weapon + 1 u per clip.
void install_rifle_armory(w::World &world) {
	w::WeaponTable &table = world.tables.weapons;
	table.entries.resize(6);
	w::WeaponTableEntry &rifle = table.entries[5];
	rifle.name = "WPN_TESTRIFLE";
	rifle.valid = true;
	rifle.category = 3;
	rifle.rank = 2;
	rifle.clipsize = 30;
	rifle.startrounds = 90;
	rifle.maxclips = 3;
	rifle.charfilter = 0xFF;
	rifle.teamfilter = 0x03;
	rifle.loadout_selectable = 1;
	rifle.ammo_class = "CLS_A";
	rifle.ammo_class_id = 0;
	rifle.ammo_class_count = 1;
	rifle.ammo_index = 1;
	rifle.weaponweight_fp16 = 0x30000;
	rifle.clipweight_fp16 = 0x10000;
	table.ammo_class_names = {"CLS_A"};
	table.ammo_class_caps = {1000};
}

// --------------------------------------------------------------------------
// Spawn protection (entity+292).
// --------------------------------------------------------------------------

bool check_spawn_protection_seeded_at_creation() {
	// A remote joiner: the add's body is hidden with no protection; its C2S 0x0B's join
	// stores the 620 after the 0x0F, whatever the spawn-zone hold (D-NET-503).
	// [orig: Entity_SpawnFromAnimSlotProperty @0x43C433 / @0x43C508; Server_OnPlayerJoin
	//  @0x51A882]
	{
		inmatch::NapiNPServerCtx ctx;
		inmatch::set_connection_mode(ctx, inmatch::ConnectionMode::HostOnly);
		ctx.is_in_session = 1;
		ctx.config.max_players = 4;
		w::World world;
		world.rules.mp_session = true;
		world.registry.configure_pool(0, 8);
		world.registry.configure_pool(3, 8);
		ctx.world = &world;
		ns::UdpSessionTransport transport(ns::UdpSessionTransport::Role::Host);
		inmatch::NapiNPConnection conn;
		conn.peer = {0x0100007Fu, 35001};
		conn.type = 1;
		conn.connection_id = inmatch::kFirstJoinerDcb;
		conn.link.mode = ns::TransportMode::Client;
		conn.link.transport = &transport;
		ctx.np_protocol.connection_list.push_back(std::move(conn));
		inmatch::NapiNPConnection &live = ctx.np_protocol.connection_list.front();
		const w::EntityHandle player =
				inmatch::Server_BuildPlayerInfoAndAdd(ctx, live, world, /*round_init=*/false);
		const w::Entity *entity = world.registry.get(player);
		if (!expect(entity != nullptr && (entity->flags & 1u) != 0 && entity->damage_state == 0,
				"a remote add's body is hidden, with no spawn protection"))
			return false;
		std::vector<ProtocolMessage> join_replies;
		inmatch::Server_HandlePlayerJoinRequest(&ctx, live, &world, /*now_tick=*/1, {0x00}, join_replies);
		entity = world.registry.get(player);
		if (!expect(entity != nullptr && entity->damage_state == 620,
				"a zone-less joiner's join stores the 620-tick protection"))
			return false;
		if (!expect(live.link.armory_reuse_seconds == 0 && !live.link.preround_loadout_latch,
				"the join zeroes the armory cooldown and clears the latch outside pre-round"))
			return false;
	}
	// The listen host's own player takes the same 620 at its join, its own C2S 0x0B's.
	{
		w::World world;
		world.registry.configure_pool(0, 8);
		world.registry.configure_pool(3, 8);
		ns::LoopbackChannel loopback;
		inmatch::NapiNPServerCtx ctx;
		inmatch::GameConfig settings;
		settings.max_players = 4;
		settings.start_delay = 7;
		inmatch::test::bring_up_host(ctx, inmatch::ConnectionMode::HostClient,
				inmatch::SocketMode::Socketless, 0, &loopback, settings);
		ctx.world = &world;
		inmatch::Server_InitNewRoundState(ctx);
		if (!expect(inmatch::Server_ProcessPendingPlayerSpawns(ctx, world) == 1,
				"host-own creation fixture spawns the local player"))
			return false;
		inmatch::NapiNPConnection &own = ctx.np_protocol.connection_list.front();
		std::vector<inmatch::InitialStateMessage> own_burst;
		(void)inmatch::test::drive_host_own_burst_and_join(ctx, own, /*now_tick=*/1, own_burst);
		const w::Entity *host = world.registry.get(world.cached.local_player);
		if (!expect(own.burst.spawned && host != nullptr && host->damage_state == 620,
				"the host's own player joins under the 620-tick protection"))
			return false;
		return expect(own.link.preround_loadout_latch,
				"a pre-round join sets the loadout latch");
	}
}

bool check_spawn_protection_countdown_and_gates() {
	inmatch::NapiNPServerCtx ctx;
	inmatch::set_connection_mode(ctx, inmatch::ConnectionMode::HostOnly);
	ctx.is_in_session = 1;
	ctx.network_quality_broadcast_countdown = 100;
	ctx.config.game_type = game_type::kTeamDeathmatch;
	w::World world;
	world.rules.mp_session = true;
	world.registry.configure_pool(0, 8);
	w::MatchRules rules;
	rules.game_type = ctx.config.game_type;
	world.match.configure(rules);
	ctx.world = &world;
	const w::EntityHandle player = w::spawn_remote_player(world, player_spawn(0, 0, 0));
	if (!expect(player.valid(), "countdown fixture spawned its player")) return false;
	ns::UdpSessionTransport transport(ns::UdpSessionTransport::Role::Host);
	ctx.np_protocol.connection_list.push_back(
			make_seeded_conn(3, 1, &transport, ns::TransportMode::Client, player, true));
	inmatch::NapiNPConnection &conn = ctx.np_protocol.connection_list.front();
	w::Entity *entity = world.registry.get(player);
	entity->damage_state = 620;

	inmatch::Server_TickUpdate(ctx);
	if (!expect(entity->damage_state == 619, "one authority tick counts the protection down by one"))
		return false;
	world.preround_delay_seconds = 3;
	inmatch::Server_TickUpdate(ctx);
	if (!expect(entity->damage_state == 619, "the pre-round timer freezes the countdown"))
		return false;
	world.preround_delay_seconds = 0;
	entity->damage_state = 2;
	inmatch::Server_TickUpdate(ctx);
	inmatch::Server_TickUpdate(ctx);
	inmatch::Server_TickUpdate(ctx);
	if (!expect(entity->damage_state == 0, "the countdown stops at zero"))
		return false;

	conn.link.spectator = true;
	entity->damage_state = 620;
	inmatch::Server_TickUpdate(ctx);
	if (!expect(entity->damage_state == -1, "a spectator slot is held at the -1 sentinel"))
		return false;
	conn.link.spectator = false;

	entity->damage_state = 400;
	world.rules.mp_session = false;
	inmatch::Server_TickUpdate(ctx);
	if (!expect(entity->damage_state == 0, "outside a live session (SP) the arm zeroes the state"))
		return false;
	world.rules.mp_session = true;

	entity->damage_state = 400;
	world.process_round_end(1);
	inmatch::Server_TickUpdate(ctx);
	drain(transport);
	return expect(entity->damage_state == 400, "the round-over latch freezes the countdown");
}

bool check_spawn_protection_cleared_by_validated_fire() {
	w::World world;
	world.rules.mp_session = true;
	world.registry.configure_pool(0, 8);
	install_rifle_armory(world);
	const w::EntityHandle shooter = w::spawn_remote_player(world, player_spawn(0, 0, 0));
	const w::EntityHandle other = w::spawn_remote_player(world, player_spawn(1, 0, 0));
	std::vector<inmatch::NapiNPConnection> roster;
	roster.push_back(make_seeded_conn(3, 1, nullptr, ns::TransportMode::Client, shooter, true));
	roster.push_back(make_seeded_conn(4, 1, nullptr, ns::TransportMode::Client, other, true));
	inmatch::NapiNPConnection &conn = roster[0];
	slots_of(world, conn)[3 * 65 + 2] = w::PersonalWeaponSlot{5, 30}; // the grant's rifle
	w::Entity *entity = world.registry.get(shooter);
	entity->damage_state = 500;
	uint32_t client_tick = conn.tick_seed;
	auto dispatch_fire = [&](const std::vector<uint8_t> &body) {
		std::vector<ProtocolMessage> msgs;
		msgs.push_back(make_protocol_message(c2s::FIRED_ROUND, body));
		(void)inmatch::dispatch_session_replies(inmatch::GameConfig{}, conn, msgs, 100, roster, &world);
	};
	// A spoofed shooter is rejected before the clear.
	dispatch_fire(fire_body(conn, other.packed, 5, client_tick));
	if (!expect(world.out.rounds.count == 0 && entity->damage_state == 500,
			"a rejected fire leaves the protection intact"))
		return false;
	// Alt fire returns through RoundData_AddRound ahead of the clear
	// [orig: Server_ClientFiredRound @0x50BE4B..0x50BE65].
	{
		ClientFiredRound alt;
		alt.current_tick = ++client_tick;
		alt.shooter_handle = shooter.packed;
		alt.target_handle = 0xFFFF;
		alt.adm_index = 5;
		alt.fire_flags = 0x23; // bit 0: alt fire
		dispatch_fire(encode_client_fired_round(alt));
	}
	if (!expect(world.out.rounds.count == 1 && entity->damage_state == 500,
			"an alt-fire round keeps the protection"))
		return false;
	dispatch_fire(fire_body(conn, shooter.packed, 5, client_tick));
	if (!expect(world.out.rounds.count == 2 && entity->damage_state == 0,
			"the first validated fire clears the protection"))
		return false;
	// Outside a session the clear is not the fire's concern (the tick arm owns it).
	world.rules.mp_session = false;
	entity->damage_state = 77;
	dispatch_fire(fire_body(conn, shooter.packed, 5, client_tick));
	return expect(world.out.rounds.count == 3 && entity->damage_state == 77,
			"the fire clear is gated on the live session");
}

bool check_deploy_seeds_protection_and_armory_state() {
	w::World world;
	world.rules.mp_session = true;
	world.registry.configure_pool(0, 8);
	world.registry.configure_pool(3, 8);
	const w::EntityHandle player = w::spawn_remote_player(world, player_spawn(0, 0, 0));
	inmatch::NapiNPConnection conn =
			make_seeded_conn(3, 1, nullptr, ns::TransportMode::Client, player, true);
	w::Entity *entity = world.registry.get(player);
	entity->alive = false;
	entity->health = 0;
	entity->flags |= w::kEntityFlagDead;
	entity->damage_state = -1;
	conn.link.armory_reuse_seconds = 12;
	conn.link.preround_loadout_latch = false;
	conn.link.death_cause_revivable = true;
	inmatch::GameConfig config;
	world.preround_delay_seconds = 4;
	const std::vector<ProtocolMessage> replies =
			inmatch::Server_ReleasePlayerDeployment(config, conn, world, {}, 0xFFFF);
	if (!expect(!replies.empty() && entity->health > 0 && entity->damage_state == 620,
			"the deploy leg seeds the 620-tick protection after the spawn-state reset"))
		return false;
	if (!expect(conn.link.armory_reuse_seconds == 0 && conn.link.preround_loadout_latch &&
			!conn.link.death_cause_revivable,
			"the deploy leg zeroes the armory cooldown, re-arms the latch in pre-round and drops the cause latch"))
		return false;
	world.preround_delay_seconds = 0;
	entity->alive = false;
	entity->health = 0;
	entity->flags |= w::kEntityFlagDead;
	(void)inmatch::Server_ReleasePlayerDeployment(config, conn, world, {}, 0xFFFF);
	return expect(!conn.link.preround_loadout_latch,
			"a deploy outside pre-round leaves the latch clear");
}

// The WAC humans count walks pool 0: a live row with the Player bit that is
// not hidden counts, a dead one included. A player still waiting to deploy
// (hidden) and an NPC do not, and no connection is needed. The def test is
// the walk's allocated-row test: every retail spawn links an items.def row,
// row 0 when the type has none, so a player whose type items.def lacks (the
// has_item_def-clear row, and the real spawn over such a table) counts too.
// [orig: Server_BuildEntitySlotLists @0x4f97a0 -- the def test @0x4F9809,
// `test eax,100h` @0x4F9815, `test bl,al` @0x4F9820, +1 @0x4f98b1; the link
// Entity_SpawnFromAnimSlotProperty @0x43C429; the miss
// ItemList_FindIndexByTypeId @0x49E131]
bool check_humans_count_the_visible_players() {
	w::World world;
	world.registry.configure_pool(0, 8);
	auto spawn = [&](bool def, uint32_t flags) {
		w::Entity e;
		e.kind = w::EntityKind::Organic;
		e.has_item_def = def;
		e.engine_flags = flags;
		e.alive = true;
		e.health = 100;
		world.registry.spawn(0, e);
	};
	spawn(true, w::kEntityFlagPlayer);
	spawn(true, w::kEntityFlagPlayer | w::kEntityFlagCarried);
	spawn(true, 0);
	spawn(false, w::kEntityFlagPlayer);
	spawn(true, w::kEntityFlagPlayer | w::kEntityFlagDead);
	inmatch::NapiNPServerCtx ctx;
	ctx.world = &world;
	ctx.is_authority = 1;
	inmatch::Server_TickUpdate(ctx);
	if (!expect(world.cached.humans == 3,
			"the humans count takes the three visible players, the def-less one included"))
		return false;
	// The traits sweep over an items table without the Player row leaves the
	// template def-less; the player spawned from it is still a human.
	world.tables.player.has_item_def = false;
	const w::EntityHandle joined = w::spawn_remote_player(world, player_spawn(4, 0, 0));
	const w::Entity *joined_row = world.registry.get(joined);
	if (!expect(joined_row != nullptr && !joined_row->has_item_def,
			"the fixture player spawns without a matched items.def row"))
		return false;
	inmatch::Server_TickUpdate(ctx);
	return expect(world.cached.humans == 4,
			"a player whose type items.def lacks counts as a human");
}

// The live group recount runs in the server tick's periodic second, ahead of
// the frame's entity update: a round that kills a group member during the
// periodic frame's entity update is counted only at the next periodic second.
// [orig: Server_TickUpdate -- the EntityPool_RecountLiveByGroup call @0x51DC02;
//  Game_ProcessMainFrame -- the Entity_UpdateAllEntities call @0x52674B
//  follows the Server_TickUpdate call @0x5266B6]
bool check_recount_precedes_the_entity_update() {
	w::World world;
	world.registry.configure_pool(0, 8);
	world.tables.ammo.entries.resize(2);
	w::AmmoTableEntry &rifle = world.tables.ammo.entries[1];
	rifle.velocity = 800;
	rifle.max_age_ticks = 124;
	rifle.weight_in_grains = 875;
	rifle.min_damage = 10;
	rifle.max_damage = 40;
	rifle.valid = true;
	w::Entity shooter_seed;
	shooter_seed.team = 1;
	const w::EntityHandle shooter = world.registry.spawn(0, shooter_seed);
	auto spawn_member = [&](float x, uint16_t net_id) {
		w::Entity seed;
		seed.kind = w::EntityKind::Organic;
		seed.has_item_def = true;
		seed.item_type = 3;
		seed.team = 2;
		seed.health = 5;
		seed.health_max = 100;
		seed.alive = true;
		seed.net_id = net_id;
		seed.group_id = 7;
		seed.position = {x, 0.0f, 0.0f};
		const w::EntityHandle h = world.registry.spawn(0, seed);
		w::AiEntity &body = *world.ai.at(world.ai.attach(h));
		body.inf.active = true;
		body.health = 5;
		body.team = 2;
		body.net_id = net_id;
		body.self->position.x.q16 = static_cast<int32_t>(x * 65536.0f);
		return h;
	};
	const w::EntityHandle victim = spawn_member(5.0f, 0x21);
	spawn_member(-200.0f, 0x22);
	inmatch::NapiNPServerCtx ctx;
	ctx.world = &world;
	ctx.is_authority = 1;
	inmatch::Server_TickUpdate(ctx); // the first periodic second
	if (!expect(world.script.relations.group(7).live_count == 2, "both members counted live"))
		return false;
	for (int tick = 1; tick < 62; ++tick) inmatch::Server_TickUpdate(ctx);
	w::RoundSpawnParams shot;
	shot.owner = shooter;
	shot.shooter_handle = shooter.packed;
	shot.origin = {0.0f, 0.0f, 0.9f};
	shot.ammo_index = 1;
	if (!expect(world.round_sim.spawn(world, shot) >= 0, "the round enters the pool"))
		return false;
	inmatch::Server_TickUpdate(ctx); // the next periodic second; the round kills after it
	const w::Entity *hit = world.registry.get(victim);
	if (!expect(hit == nullptr || hit->health <= 0, "the round kills in this frame's entity update") ||
			!expect(world.script.relations.group(7).live_count == 2,
					"the periodic recount ran before the kill"))
		return false;
	for (int tick = 0; tick < 62; ++tick) inmatch::Server_TickUpdate(ctx);
	return expect(world.script.relations.group(7).live_count == 1,
			"the following periodic second counts the kill");
}

// The pending fire-sound slots count down at the head of the host's frame,
// ahead of the server tick's receive, so the server tick itself never counts
// them: a slot queued before it still needs its full countdown afterwards.
// [orig: Game_ProcessMainFrame -- the Sound_TickPendingSlots call @0x526697
//  precedes the Server_TickUpdate call @0x5266B6 (its receive pump @0x51D895)]
bool check_server_tick_leaves_the_pending_sounds() {
	w::World world;
	world.registry.configure_pool(0, 2);
	world.out.fire_sounds.set_listener({0.0f, 0.0f, 0.0f});
	// 330 units away: (62 * 330 / 330) >> 2 = 15 ticks.
	world.out.fire_sounds.play_with_distance_delay("CRACK", {330.0f, 0.0f, 0.0f}, 0);
	if (!expect(world.out.fire_sounds.pending_count() == 1, "the far report waits in a slot"))
		return false;
	inmatch::NapiNPServerCtx ctx;
	ctx.world = &world;
	ctx.is_authority = 1;
	inmatch::Server_TickUpdate(ctx);
	for (int tick = 0; tick < 14; ++tick) world.out.fire_sounds.tick();
	if (!expect(world.out.fire_sounds.drain().empty(),
			"the server tick left the countdown untouched")) return false;
	world.out.fire_sounds.tick();
	return expect(world.out.fire_sounds.drain().size() == 1,
			"the fifteenth frame-head tick plays it");
}

// --------------------------------------------------------------------------
// The 1 Hz round-robin 0x2F flag refresh.
// --------------------------------------------------------------------------

bool check_flag_refresh_round_robin() {
	w::World world;
	world.rules.mp_session = true;
	world.registry.configure_pool(0, 4);
	world.registry.configure_pool(1, 8);
	w::MatchRules rules;
	rules.game_type = game_type::kCaptureTheFlag;
	world.match.configure(rules);
	const w::EntityHandle player = w::spawn_remote_player(world, player_spawn(10, 20, 2));
	auto spawn_flag = [&](int32_t item_id, float x) {
		w::Entity flag;
		flag.kind = w::EntityKind::Item;
		flag.item_id = item_id;
		flag.has_item_def = true;
		flag.alive = true;
		flag.position = {x, 5.0f, 2.0f};
		flag.spawn_position = flag.position;
		return world.registry.spawn(1, flag);
	};
	// A non-flag pool-1 row between the two flags must not count.
	const w::EntityHandle flag_a = spawn_flag(4091, 100.0f);
	w::Entity crate;
	crate.kind = w::EntityKind::Item;
	crate.item_id = 7001;
	crate.has_item_def = true;
	world.registry.spawn(1, crate);
	const w::EntityHandle flag_b = spawn_flag(4093, 200.0f);

	inmatch::NapiNPServerCtx ctx;
	ctx.world = &world;
	ctx.is_authority = 1;
	ctx.is_in_session = 1;
	ctx.config.game_type = rules.game_type;
	ns::UdpSessionTransport remote(ns::UdpSessionTransport::Role::Host);
	ns::LoopbackChannel host_wire;
	ctx.np_protocol.connection_list.push_back(
			make_seeded_conn(3, 1, &remote, ns::TransportMode::Client, player, true));
	const w::EntityHandle host = w::spawn_player(world, player_spawn(0, 0, 0));
	ctx.np_protocol.connection_list.push_back(
			make_seeded_conn(1, 2, &host_wire, ns::TransportMode::Loopback, host, true));

	auto refresh_this_second = [&](const char *label, w::EntityHandle expected,
			uint16_t expected_attach) {
		tick_to_periodic_second(ctx);
		const std::vector<ObjectiveEntityState> states = objective_states(drain(remote));
		const std::vector<ObjectiveEntityState> host_states = objective_states(drain(host_wire));
		if (!expected.valid()) {
			if (!expect(states.empty() && host_states.empty(), label)) return false;
			return true;
		}
		return expect(states.size() == 1 && host_states.size() == 1 &&
						states[0].entity_handle == expected.packed &&
						states[0].attach_handle == expected_attach &&
						states[0].ground_handle == 0xFFFF &&
						host_states[0].entity_handle == expected.packed,
				label);
	};
	if (!refresh_this_second("second 1 refreshes flag A (resting, no attach)", flag_a, 0xFFFF))
		return false;
	// Carry flag B: the refresh publishes the carrier as its attach.
	world.registry.get(player)->mounted_child = flag_b;
	world.registry.get(flag_b)->primary_occupant = player;
	world.registry.get(flag_b)->flags |= w::kEntityFlagCarried;
	if (!refresh_this_second("second 2 refreshes flag B with its carrier attached", flag_b,
			player.packed))
		return false;
	if (!refresh_this_second("the walk past the last flag sends nothing and resets the cursor",
			{}, 0))
		return false;
	if (!refresh_this_second("second 4 wraps back to flag A", flag_a, 0xFFFF))
		return false;
	// The refresh is not gated on the round-over latch.
	world.process_round_end(1);
	drain(remote);
	drain(host_wire);
	if (!refresh_this_second("the refresh continues after the round ends", flag_b, player.packed))
		return false;
	// Without any flag entity nothing is ever sent.
	world.registry.despawn(flag_a);
	world.registry.despawn(flag_b);
	world.registry.get(player)->mounted_child = {};
	world.flag_refresh_cursor = 0;
	return refresh_this_second("no flag entity, no 0x2F", {}, 0);
}

// --------------------------------------------------------------------------
// The violation sweep's carry limit.
// --------------------------------------------------------------------------

bool check_flag_carry_limit_breaks_the_carry_and_kills(uint32_t game_type_value,
		bool expect_kill, bool uplinked = false) {
	w::World world;
	world.rules.mp_session = true;
	world.registry.configure_pool(0, 4);
	world.registry.configure_pool(1, 4);
	w::MatchRules rules;
	rules.game_type = game_type_value;
	world.match.configure(rules);
	const w::Vec3 carrier_position{30.0f, 40.0f, 2.0f};
	const w::EntityHandle carrier = w::spawn_remote_player(world,
			player_spawn(carrier_position.x, carrier_position.y, carrier_position.z));
	world.match.upsert_player({carrier, 3, "Carrier"});
	// A carrier that has uplinked runs the remote motor leg, which latches no
	// dead bit [orig: the org2 edge's latch @0x4B4CB5 is the local leg's alone].
	if (uplinked) world.ai.for_handle(carrier)->net_is_remote_peer = true;
	w::Entity flag_seed;
	flag_seed.kind = w::EntityKind::Item;
	flag_seed.item_id = 4091;
	flag_seed.has_item_def = true;
	flag_seed.alive = true;
	flag_seed.spawn_position = {5.0f, 6.0f, 2.0f};
	flag_seed.position = carrier_position;
	flag_seed.primary_occupant = carrier;
	flag_seed.flags = w::kEntityFlagCarried;
	const w::EntityHandle flag = world.registry.spawn(1, flag_seed);
	world.registry.get(carrier)->mounted_child = flag;

	inmatch::NapiNPServerCtx ctx;
	ctx.world = &world;
	ctx.is_authority = 1;
	ctx.is_in_session = 1;
	ctx.config.game_type = game_type_value;
	ctx.config.flag_reset_seconds = 3;
	ctx.config.permanent_death = true; // keep the dead slot past the t7 punt
	ns::UdpSessionTransport remote(ns::UdpSessionTransport::Role::Host);
	ctx.np_protocol.connection_list.push_back(
			make_seeded_conn(3, 1, &remote, ns::TransportMode::Client, carrier, true));
	inmatch::NapiNPConnection &conn = ctx.np_protocol.connection_list.front();

	tick_to_periodic_second(ctx);
	drain(remote);
	if (!expect(conn.link.flag_carry_seconds == (expect_kill ? 1u : 0u) &&
			world.registry.get(carrier)->health > 0,
			"the first carried second counts only in the flag modes"))
		return false;
	tick_to_periodic_second(ctx);
	drain(remote);
	if (!expect(conn.link.flag_carry_seconds == (expect_kill ? 2u : 0u) &&
			world.registry.get(carrier)->mounted_child == flag,
			"the second carried second keeps counting below the limit"))
		return false;
	tick_to_periodic_second(ctx);
	const std::vector<ns::Datagram> limit_second = drain(remote);
	if (!expect_kill) {
		return expect(conn.link.flag_carry_seconds == 0 &&
						world.registry.get(carrier)->health > 0 &&
						world.registry.get(carrier)->mounted_child == flag,
				"a non-flag game type never counts or breaks the carry");
	}
	w::Entity *body = world.registry.get(carrier);
	const w::Entity *flag_entity = world.registry.get(flag);
	if (!expect(conn.link.flag_carry_seconds == 0 && body->health == -1 &&
			!body->mounted_child.valid() && !flag_entity->primary_occupant.valid() &&
			flag_entity->position.x == 5.0f && flag_entity->position.y == 6.0f,
			"at the limit the counter resets, the carry breaks, the flag snaps home and Health is -1"))
		return false;
	// The sweep's drop and re-sync send their 0x2F inline, ahead of the same
	// second's refresh, which publishes the home pose again (D-NET-NEW-V3-1)
	// [orig: Server_CheckPlayerViolations - Entity_DropCarriedObject @0x51AC84 (its
	//  0x2F @0x439F34), Entity_SyncPositionFromDefinition @0x51AC8A; then
	//  Server_RefreshNextFlagState @0x51DF64].
	const std::vector<ObjectiveEntityState> limit_states = objective_states(limit_second);
	if (!expect(limit_states.size() == 3 && limit_states[0].entity_handle == flag.packed &&
			limit_states[0].attach_handle == 0xFFFF &&
			limit_states[0].pos_x == w::to_fixed(carrier_position.x) &&
			limit_states[0].pos_y == w::to_fixed(carrier_position.y) &&
			limit_states[1].entity_handle == flag.packed && limit_states[1].attach_handle == 0xFFFF &&
			limit_states[1].pos_x == 5 * 65536 && limit_states[1].pos_y == 6 * 65536 &&
			limit_states[2].entity_handle == flag.packed &&
			limit_states[2].pos_x == 5 * 65536 && limit_states[2].pos_y == 6 * 65536,
			"the limit second fans the drop-pose 0x2F, the home-pose 0x2F, then the refresh's"))
		return false;
	if (!expect(!saw_flag_return_feed(limit_second),
			"the carry break never emits a 0x23..0x25 return feed"))
		return false;
	inmatch::Server_TickUpdate(ctx); // the limit second's entity-update sends lead it
	const std::vector<ns::Datagram> next_tick = drain(remote);
	if (!expect(objective_states(next_tick).empty() && !saw_flag_return_feed(next_tick),
			"nothing of the carry break waits for the next tick but the death"))
		return false;
	bool saw_death = false;
	for (const ns::Datagram &d : next_tick)
		if (d.tag == s2c::ENTITY_DEATH && d.body.size() == 4 &&
				(d.body[0] | (d.body[1] << 8)) == carrier.packed)
			saw_death = true;
	return expect(saw_death && !body->alive && (body->flags & w::kEntityFlagDead) != 0,
			"the carrier dies through the ordinary death transaction");
}

// The flag reset's death is the player body's edge on its Health -1, raised in
// the same frame's entity update, after the tick's send pump and ahead of the
// next receive pump (D-NET-550; the edge's own since D-AI-9's org2 half, its
// sends leading the next frame's queue). A C2S 0x4D in that next pump therefore kills
// the already dead body a second time (the team change has no dead gate) and
// its penalty holds stand last; a C2S 0x41 there stores its unfloored timeout
// over the death's floored holds and raises nothing (the body is dead).
// [orig: Server_CheckPlayerViolations -- Health -1 @0x51AC94; the edge
//  Entity_UpdateInfantryPlayerBody @0x4B4BF1..0x4B4CEA, run by
//  Entity_UpdateAllEntities @0x52674B after Server_TickUpdate @0x5266B6;
//  NapiNPServerMsg_0x04D_ChangeTeam @0x518F70 -- GameEvent_PlayerDeath
//  @0x519059 / @0x5190C7, +360 @0x51906B / @0x5190D9;
//  NapiNPServerMsg_DeathTimeoutReset -- +360 @0x51057B, +364 @0x510587]
bool check_flag_reset_dies_ahead_of_the_next_pump(uint8_t next_pump_tag, bool uplinked) {
	w::World world;
	world.rules.mp_session = true;
	world.rules.logic_authority = true;
	world.registry.configure_pool(0, 4);
	world.registry.configure_pool(1, 4);
	w::MatchRules rules;
	rules.game_type = game_type::kCaptureTheFlag;
	world.match.configure(rules);
	const w::EntityHandle carrier = w::spawn_remote_player(world, player_spawn(30.0f, 40.0f, 2.0f));
	world.match.upsert_player({carrier, 3, "Carrier"});
	if (uplinked) world.ai.for_handle(carrier)->net_is_remote_peer = true; // the remote motor leg
	w::Entity flag_seed;
	flag_seed.kind = w::EntityKind::Item;
	flag_seed.item_id = 4091;
	flag_seed.has_item_def = true;
	flag_seed.alive = true;
	flag_seed.spawn_position = {5.0f, 6.0f, 2.0f};
	flag_seed.position = {30.0f, 40.0f, 2.0f};
	flag_seed.primary_occupant = carrier;
	flag_seed.flags = w::kEntityFlagCarried;
	const w::EntityHandle flag = world.registry.spawn(1, flag_seed);
	world.registry.get(carrier)->mounted_child = flag;

	inmatch::NapiNPServerCtx ctx;
	ctx.world = &world;
	ctx.is_authority = 1;
	ctx.is_in_session = 1;
	ctx.config.game_type = game_type::kCaptureTheFlag;
	ctx.config.flag_reset_seconds = 1;
	ctx.config.permanent_death = true; // keep the dead slot past the t7 punt
	ctx.config.respawn_timeout = 1;
	ctx.config.change_team_penalty_seconds = 60;
	ctx.config.mp_attributes |= inmatch::GameConfig::kMpAttribTeamChoose;
	ns::UdpSessionTransport remote(ns::UdpSessionTransport::Role::Host);
	ctx.np_protocol.connection_list.push_back(
			make_seeded_conn(3, 1, &remote, ns::TransportMode::Client, carrier, true));
	inmatch::NapiNPConnection &conn = ctx.np_protocol.connection_list.front();
	int deaths = 0;
	const auto count_deaths = [&]() {
		for (const ns::Datagram &d : drain(remote))
			if (d.tag == s2c::ENTITY_DEATH && d.body.size() == 4 &&
					(d.body[0] | (d.body[1] << 8)) == carrier.packed)
				++deaths;
	};

	tick_to_periodic_second(ctx); // the limit second: Health -1, then its entity update
	count_deaths();
	const bool tail_death = expect(
			world.registry.get(carrier)->health == -1 && deaths == 0 &&
					(world.registry.get(carrier)->flags & w::kEntityFlagDead) != 0 &&
					conn.link.respawn_delay_seconds == 3,
			"flag-reset: the limit second's entity update raises the death ahead of the "
			"next pump, its sends staged for the next frame");
	inmatch::ServerDispatchInputs inputs;
	inputs.server_ctx = &ctx;
	(void)inmatch::dispatch_session_replies(ctx.config, conn,
			{make_protocol_message(next_pump_tag, {})}, 100,
			ctx.np_protocol.connection_list, &world, inputs);
	inmatch::Server_TickUpdate(ctx);
	count_deaths();
	if (next_pump_tag == c2s::TEAM_CHANGE_REQUEST)
		return expect(deaths == 2 && world.registry.get(carrier)->team == 2 &&
						conn.link.respawn_delay_seconds >= 59,
				"flag-reset: the next pump's team change kills again and its penalty stands") &&
				tail_death;
	return expect(deaths == 1 && conn.link.respawn_delay_seconds == 1 &&
					conn.link.spawn_target_hold_seconds == 1,
			"flag-reset: the next pump's 0x41 stores its unfloored timeout over the death's holds") &&
			tail_death;
}

// The flag reset's death credits the carrier's +0x178 as the edge leaves it:
// the edge clears it unless a non-lethal hit staged the death selection
// (+0x2C0), so a carrier an enemy wounded first scores that enemy a kill.
// [orig: Entity_UpdateInfantryPlayerBody @0x4B4C72..0x4B4C8D; GameEvent_PlayerDeath
//  reads +0x178 @0x516F47, the scoring @0x516FB0]
bool check_flag_reset_credits_a_staged_attacker(bool staged) {
	w::World world;
	world.rules.mp_session = true;
	world.rules.logic_authority = true;
	world.registry.configure_pool(0, 4);
	world.registry.configure_pool(1, 4);
	w::MatchRules rules;
	rules.game_type = game_type::kCaptureTheFlag;
	world.match.configure(rules);
	const w::EntityHandle carrier = w::spawn_remote_player(world, player_spawn(30.0f, 40.0f, 2.0f));
	const w::EntityHandle shooter =
			w::spawn_remote_player(world, player_spawn(60.0f, 40.0f, 2.0f, /*team=*/2));
	world.match.upsert_player({carrier, 3, "Carrier"});
	world.match.upsert_player({shooter, 4, "Shooter"});
	w::Entity flag_seed;
	flag_seed.kind = w::EntityKind::Item;
	flag_seed.item_id = 4091;
	flag_seed.has_item_def = true;
	flag_seed.alive = true;
	flag_seed.spawn_position = {5.0f, 6.0f, 2.0f};
	flag_seed.position = {30.0f, 40.0f, 2.0f};
	flag_seed.primary_occupant = carrier;
	flag_seed.flags = w::kEntityFlagCarried;
	const w::EntityHandle flag = world.registry.spawn(1, flag_seed);
	w::Entity *body = world.registry.get(carrier);
	body->mounted_child = flag;
	// The earlier non-lethal hit: its shooter and, if staged, its death clip.
	body->last_attacker = shooter;
	body->death_anim_state = staged
			? w::compute_death_anim_state(0, 0, w::death_cause::kGeneric)
			: 0;
	// The 64-tick think fired just before: its health tail would stage the
	// default selection for a Health <= 0 body in the reset's own update.
	// [orig: Entity_UpdateInfantryPlayerBody @0x4B4BC9..0x4B4BE9;
	//  Entity_HandleDamageAndTriggerZones @0x407ACF..0x407AE7]
	body->spawn_phase = 64;

	inmatch::NapiNPServerCtx ctx;
	ctx.world = &world;
	ctx.is_authority = 1;
	ctx.is_in_session = 1;
	ctx.config.game_type = game_type::kCaptureTheFlag;
	ctx.config.flag_reset_seconds = 1;
	ctx.config.permanent_death = true;
	ns::UdpSessionTransport remote(ns::UdpSessionTransport::Role::Host);
	ctx.np_protocol.connection_list.push_back(
			make_seeded_conn(3, 1, &remote, ns::TransportMode::Client, carrier, true));
	tick_to_periodic_second(ctx);
	const w::MatchPlayer *stats = world.match.player(shooter);
	const int32_t kills = stats != nullptr ? stats->stats[w::MatchStats::kEnemyKills] : -1;
	return expect((world.registry.get(carrier)->flags & w::kEntityFlagDead) != 0 &&
					kills == (staged ? 1 : 0),
			staged ? "flag-reset: a staged hit's shooter is credited with the kill"
			       : "flag-reset: an unstaged edge clears the attacker, so no kill is credited");
}

// A flag reset in a frame whose entity update does not run (a host that also
// plays, its own client's copy of the pre-round byte still up) leaves its edge
// waiting; a C2S 0x4D before the next entity update kills the body and latches
// its dead bit, so the waiting edge, which skips a dead body, raises no second
// death. [orig: Game_ProcessMainFrame -- `cmp g_ClientPreRoundTimer` @0x52672C;
// Entity_UpdateInfantryPlayerBody -- the dead bit @0x4B4BFE;
// NapiNPServerMsg_0x04D_ChangeTeam -- Flags |= 2 @0x519061 / @0x5190CF]
bool check_flag_reset_waiting_edge_skips_a_dead_body() {
	w::World world;
	world.rules.mp_session = true;
	world.rules.logic_authority = true;
	world.registry.configure_pool(0, 4);
	world.registry.configure_pool(1, 4);
	w::MatchRules rules;
	rules.game_type = game_type::kCaptureTheFlag;
	world.match.configure(rules);
	const w::EntityHandle carrier = w::spawn_remote_player(world, player_spawn(30.0f, 40.0f, 2.0f));
	world.match.upsert_player({carrier, 3, "Carrier"});
	w::Entity flag_seed;
	flag_seed.kind = w::EntityKind::Item;
	flag_seed.item_id = 4091;
	flag_seed.has_item_def = true;
	flag_seed.alive = true;
	flag_seed.spawn_position = {5.0f, 6.0f, 2.0f};
	flag_seed.position = {30.0f, 40.0f, 2.0f};
	flag_seed.primary_occupant = carrier;
	flag_seed.flags = w::kEntityFlagCarried;
	const w::EntityHandle flag = world.registry.spawn(1, flag_seed);
	world.registry.get(carrier)->mounted_child = flag;
	world.preround_delay_seconds = 2;
	world.cached.client_preround_timer = 2; // the own client's copy holds the update

	inmatch::NapiNPServerCtx ctx;
	ctx.world = &world;
	ctx.is_authority = 1;
	ctx.is_in_session = 1;
	ctx.config.game_type = game_type::kCaptureTheFlag;
	ctx.config.flag_reset_seconds = 1;
	ctx.config.permanent_death = true;
	ctx.config.change_team_penalty_seconds = 60;
	ctx.config.mp_attributes |= inmatch::GameConfig::kMpAttribTeamChoose;
	ns::UdpSessionTransport remote(ns::UdpSessionTransport::Role::Host);
	ctx.np_protocol.connection_list.push_back(
			make_seeded_conn(3, 1, &remote, ns::TransportMode::Client, carrier, true));
	inmatch::NapiNPConnection &conn = ctx.np_protocol.connection_list.front();
	int deaths = 0;
	const auto count_deaths = [&]() {
		for (const ns::Datagram &d : drain(remote))
			if (d.tag == s2c::ENTITY_DEATH && d.body.size() == 4 &&
					(d.body[0] | (d.body[1] << 8)) == carrier.packed)
				++deaths;
	};

	tick_to_periodic_second(ctx); // the limit second, in the pre-round
	count_deaths();
	if (!expect(world.registry.get(carrier)->health == -1 && deaths == 0 &&
					(world.registry.get(carrier)->flags & w::kEntityFlagDead) == 0,
			"waiting-edge: the pre-round limit second stores Health -1 and its edge waits"))
		return false;
	inmatch::ServerDispatchInputs inputs;
	inputs.server_ctx = &ctx;
	(void)inmatch::dispatch_session_replies(ctx.config, conn,
			{make_protocol_message(c2s::TEAM_CHANGE_REQUEST, {})}, 100,
			ctx.np_protocol.connection_list, &world, inputs);
	count_deaths();
	if (!expect(deaths == 1, "waiting-edge: the team change kills the waiting body"))
		return false;
	// The pre-round ends and the own client's copy clears: the next entity
	// update runs and its edge skips the dead body. (While the pre-round runs,
	// each 0x0A clears the dead bit and the edge re-kills the body, D-NET-439:
	// npruntime_player_state_writer.)
	world.preround_delay_seconds = 0;
	world.cached.client_preround_timer = 0;
	const uint32_t updates = world.entity_update_counter;
	inmatch::Server_TickUpdate(ctx);
	inmatch::Server_TickUpdate(ctx);
	count_deaths();
	return expect(world.entity_update_counter > updates && deaths == 1 &&
					conn.link.respawn_delay_seconds >= 55,
			"waiting-edge: the first entity update after it skips the dead body");
}

// --------------------------------------------------------------------------
// The team-mode 0x46 downed resend.
// --------------------------------------------------------------------------

bool check_team_downed_resend() {
	w::World world;
	world.rules.mp_session = true;
	world.registry.configure_pool(0, 8);
	w::MatchRules rules;
	rules.game_type = game_type::kTeamDeathmatch;
	world.match.configure(rules);
	const w::EntityHandle victim = w::spawn_remote_player(world, player_spawn(0, 0, 0, 1));
	const w::EntityHandle killer = w::spawn_remote_player(world, player_spawn(5, 0, 0, 2));
	const w::EntityHandle teammate = w::spawn_remote_player(world, player_spawn(1, 0, 0, 1));
	world.match.upsert_player({victim, 1, "Victim"});
	world.match.upsert_player({killer, 2, "Killer"});
	world.match.upsert_player({teammate, 3, "Mate"});

	inmatch::NapiNPServerCtx ctx;
	ctx.world = &world;
	ctx.is_authority = 1;
	ctx.is_in_session = 1;
	ctx.config.game_type = rules.game_type;
	ctx.config.permanent_death = true;
	ns::UdpSessionTransport victim_wire(ns::UdpSessionTransport::Role::Host);
	ns::UdpSessionTransport killer_wire(ns::UdpSessionTransport::Role::Host);
	ns::UdpSessionTransport mate_wire(ns::UdpSessionTransport::Role::Host);
	auto &roster = ctx.np_protocol.connection_list;
	roster.push_back(make_seeded_conn(3, 1, &victim_wire, ns::TransportMode::Client, victim, true));
	roster.push_back(make_seeded_conn(4, 1, &killer_wire, ns::TransportMode::Client, killer, true));
	roster.push_back(make_seeded_conn(5, 1, &mate_wire, ns::TransportMode::Client, teammate, true));
	roster[0].reply.player_slot = 1;
	roster[1].reply.player_slot = 2;
	roster[2].reply.player_slot = 3;
	const uint8_t slot_teams[] = {1, 2, 1}; // +0x1A0, as the bodies'
	for (size_t i = 0; i < roster.size(); ++i) {
		roster[i].assigned_team = slot_teams[i];
		roster[i].assigned_team_valid = true;
	}

	auto count_downed = [&](ns::UdpSessionTransport &wire, uint8_t expected_state,
			bool &shape_ok) {
		int count = 0;
		shape_ok = true;
		for (const ns::Datagram &d : drain(wire)) {
			if (d.tag != s2c::PLAYER_SYNC) continue;
			PlayerSync sync;
			if (!decode_player_sync(d.body.data(), d.body.size(), sync) ||
					sync.field_bitmask != kPlayerSyncHasDownedState)
				continue;
			++count;
			if (d.body.size() != 5 || d.reliable || sync.slot_id != 1 ||
					sync.entity_slot_id != static_cast<uint8_t>(victim.slot()) ||
					sync.downed_state != expected_state)
				shape_ok = false;
		}
		return count;
	};

	// The revivable kill: another player, no knife/headshot cause bits, raised by
	// the victim body's edge in the previous frame's entity update (the
	// transaction latches the body dead here, as the edge would have).
	world.registry.get(victim)->health = 0;
	w::RoundDeath death;
	death.victim = victim;
	death.killer = killer;
	death.victim_handle = victim.packed;
	death.killer_handle = killer.packed;
	inmatch::Server_ProcessPlayerDeath(ctx, world, death);
	// Tick 1: the window stands at 120, the shared periodic second decrements it
	// to 119, then the zero-armed resend countdown fires.
	inmatch::Server_TickUpdate(ctx);
	if (!expect(roster[0].link.death_cause_revivable &&
			roster[0].link.downed_revive_seconds == 119,
			"a plain other-player kill latches the revivable cause"))
		return false;
	bool victim_shape = true;
	bool mate_shape = true;
	bool killer_shape = true;
	int to_victim = count_downed(victim_wire, 119, victim_shape);
	int to_mate = count_downed(mate_wire, 119, mate_shape);
	int to_killer = count_downed(killer_wire, 119, killer_shape);
	if (!expect(to_victim == 1 && to_mate == 1 && to_killer == 0 && victim_shape && mate_shape,
			"the zero-armed countdown resends the 5-B field-0x0008 form to the victim's team only"))
		return false;
	// Ticks 2..62 decrement the reloaded 62; tick 63 fires again.
	for (int i = 0; i < 61; ++i) inmatch::Server_TickUpdate(ctx);
	if (!expect(count_downed(victim_wire, 119, victim_shape) == 0 &&
			count_downed(mate_wire, 119, mate_shape) == 0,
			"no resend inside the 62-tick window"))
		return false;
	inmatch::Server_TickUpdate(ctx);
	to_victim = count_downed(victim_wire, 118, victim_shape);
	to_mate = count_downed(mate_wire, 118, mate_shape);
	to_killer = count_downed(killer_wire, 118, killer_shape);
	if (!expect(to_victim == 1 && to_mate == 1 && to_killer == 0 && victim_shape && mate_shape,
			"the 62nd tick after the reload resends, tracking the revive window"))
		return false;
	for (int i = 0; i < 62; ++i) inmatch::Server_TickUpdate(ctx);
	if (!expect(count_downed(mate_wire, 117, mate_shape) == 1 && mate_shape,
			"the resend repeats every 62 ticks"))
		return false;
	// The filter is the victim SLOT's team and each candidate's slot team, no
	// body read on either side (D-NET-636): a script team write that moves a
	// body alone [orig: Entity_SetTeamByNetId @0x43C680] changes no recipient.
	// [orig: `movzx edx, [esi+1A0h]` @0x51E35D -> @0x51E36D;
	//  NapiNPServer_SendFiltered @0x4C896A..0x4C8977]
	(void)drain(victim_wire);
	(void)drain(killer_wire);
	const auto split_leg = [&](uint8_t victim_body, uint8_t mate_body, uint8_t killer_body,
			uint8_t expected_state, const char *label) {
		world.registry.get(victim)->team = victim_body;
		world.registry.get(teammate)->team = mate_body;
		world.registry.get(killer)->team = killer_body;
		for (int i = 0; i < 62; ++i) inmatch::Server_TickUpdate(ctx);
		const int victim_count = count_downed(victim_wire, expected_state, victim_shape);
		const int mate_count = count_downed(mate_wire, expected_state, mate_shape);
		const int killer_count = count_downed(killer_wire, expected_state, killer_shape);
		return expect(victim_count == 1 && mate_count == 1 && killer_count == 0 &&
						victim_shape && mate_shape,
				label);
	};
	if (!split_leg(2, 1, 2, 116,
				"a victim body moved to team 2 still resends to its slot team, not team 2"))
		return false;
	if (!split_leg(1, 2, 1, 115,
				"a split candidate receives by its slot team, not its body's"))
		return false;
	world.registry.get(teammate)->team = 1;
	world.registry.get(killer)->team = 2;

	// A headshot death is never resent.
	roster[0].link.death_cause_revivable = false;
	for (int i = 0; i < 62; ++i) inmatch::Server_TickUpdate(ctx);
	if (!expect(count_downed(mate_wire, 0, mate_shape) == 0,
			"a knife/headshot cause suppresses the resend"))
		return false;
	// Non-team game types never run the producer.
	roster[0].link.death_cause_revivable = true;
	w::MatchRules solo;
	solo.game_type = game_type::kDeathmatch;
	world.match.configure(solo);
	for (int i = 0; i < 62; ++i) inmatch::Server_TickUpdate(ctx);
	return expect(count_downed(mate_wire, 0, mate_shape) == 0,
			"a non-team game type never resends the downed state");
}

// --------------------------------------------------------------------------
// The death consumer's cause-bit read + clear (entity+44 bits 8..11).
// --------------------------------------------------------------------------

// Every 8-byte S2C 0x1E body's event byte on one wire, in send order.
std::vector<uint8_t> death_feed_types(ns::UdpSessionTransport &wire) {
	std::vector<uint8_t> out;
	for (const ns::Datagram &d : drain(wire)) {
		if (d.tag == s2c::GAME_EVENT && d.body.size() == 8) out.push_back(d.body[0]);
	}
	return out;
}

// The classifier reads the victim's LIVE cause word (latched on the body at
// hit time, never a death-record snapshot), the revive-window gate reads it
// first, and only the bit the 0x1E family reports is cleared on the body.
bool check_death_consumer_reads_and_clears_the_reported_cause_bit() {
	w::World world;
	world.rules.mp_session = true;
	world.registry.configure_pool(0, 8);
	w::MatchRules rules;
	rules.game_type = game_type::kTeamDeathmatch;
	world.match.configure(rules);
	const w::EntityHandle head = w::spawn_remote_player(world, player_spawn(0, 0, 0, 1));
	const w::EntityHandle multi = w::spawn_remote_player(world, player_spawn(2, 0, 0, 1));
	const w::EntityHandle self = w::spawn_remote_player(world, player_spawn(4, 0, 0, 1));
	const w::EntityHandle killer = w::spawn_remote_player(world, player_spawn(9, 0, 0, 2));
	world.match.upsert_player({head, 1, "Head"});
	world.match.upsert_player({multi, 2, "Multi"});
	world.match.upsert_player({self, 3, "Self"});
	world.match.upsert_player({killer, 4, "Killer"});

	inmatch::NapiNPServerCtx ctx;
	ctx.world = &world;
	ctx.is_authority = 1;
	ctx.is_in_session = 1;
	ctx.config.game_type = rules.game_type;
	ctx.config.death_messages = 1;
	ns::UdpSessionTransport head_wire(ns::UdpSessionTransport::Role::Host);
	ns::UdpSessionTransport multi_wire(ns::UdpSessionTransport::Role::Host);
	ns::UdpSessionTransport self_wire(ns::UdpSessionTransport::Role::Host);
	ns::UdpSessionTransport killer_wire(ns::UdpSessionTransport::Role::Host);
	auto &roster = ctx.np_protocol.connection_list;
	roster.push_back(make_seeded_conn(3, 1, &head_wire, ns::TransportMode::Client, head, true));
	roster.push_back(make_seeded_conn(4, 1, &multi_wire, ns::TransportMode::Client, multi, true));
	roster.push_back(make_seeded_conn(5, 1, &self_wire, ns::TransportMode::Client, self, true));
	roster.push_back(make_seeded_conn(6, 1, &killer_wire, ns::TransportMode::Client, killer, true));
	roster[0].reply.player_slot = 1;
	roster[1].reply.player_slot = 2;
	roster[2].reply.player_slot = 3;
	roster[3].reply.player_slot = 4;

	// The lethal hit: Health 0, its cause bits, its shooter (+0x178) and its
	// staged death selection (+0x2C0, which keeps the shooter through the edge).
	// The body's edge raises the transaction in the next entity update, and
	// its 0x1E leads the frame after.
	auto raise_death = [&](w::EntityHandle victim, w::EntityHandle by, uint32_t cause) {
		w::Entity *body = world.registry.get(victim);
		body->health = 0;
		body->cause_flags = cause;
		// The lethal hit re-armed the plyr think (spawnPhase = 64), so the
		// cadence clear cannot race this tick's consumer.
		body->spawn_phase = 64;
		body->last_attacker = by;
		body->death_anim_state = w::compute_death_anim_state(0, 0, w::death_cause::kGeneric);
		inmatch::Server_TickUpdate(ctx);
		inmatch::Server_TickUpdate(ctx);
	};

	// A critical (head-zone) kill: the 0x1E ships the STRCND08 family 10..12,
	// the reported 0x800 is cleared on the body, and no revive window opens.
	raise_death(head, killer, 0x800u);
	std::vector<uint8_t> feed = death_feed_types(killer_wire);
	if (!expect(feed.size() == 1 && feed[0] >= 10 && feed[0] <= 12,
			"a latched 0x800 reports the headshot family"))
		return false;
	if (!expect(world.registry.get(head)->cause_flags == 0u &&
			roster[0].link.downed_revive_seconds == 0 &&
			!roster[0].link.death_cause_revivable,
			"the consumer clears the reported 0x800; the cause suppresses the revive window"))
		return false;

	// Both 0x100 and 0x800 latched: 0x100 wins the ladder (event 32) and ONLY
	// that bit is cleared; the head-zone bit stays on the body.
	raise_death(multi, killer, 0x900u);
	feed = death_feed_types(killer_wire);
	if (!expect(feed.size() == 1 && feed[0] == 32 &&
			world.registry.get(multi)->cause_flags == 0x800u,
			"a latched 0x100 reports 32 and clears only its own bit"))
		return false;

	// The suicide branch returns before the ladder, so nothing is cleared.
	raise_death(self, self, 0x800u);
	feed = death_feed_types(killer_wire);
	return expect(feed.size() == 1 && feed[0] >= 1 && feed[0] <= 3 &&
			world.registry.get(self)->cause_flags == 0x800u,
			"a self-kill leaves the latched cause bits untouched");
}

// --------------------------------------------------------------------------
// The roster row's spectator latch (the player-slot +100567 mirror).
// --------------------------------------------------------------------------

// The runtime spectator convert and its reverse write the same roster latch
// the end-round winner award reads, so a spectator-flagged top scorer is
// skipped [orig: Server_ConvertPlayerToSpectator @0x519E76; Match::finish].
bool check_spectator_latch_mirrors_onto_the_roster_row() {
	w::World world;
	world.rules.mp_session = true;
	world.registry.configure_pool(0, 8);
	w::MatchRules rules;
	rules.game_type = game_type::kTeamDeathmatch;
	world.match.configure(rules);
	const w::EntityHandle player = w::spawn_remote_player(world, player_spawn(0, 0, 0, 1));
	world.match.upsert_player({player, 1, "Watcher"});

	inmatch::NapiNPServerCtx ctx;
	ctx.world = &world;
	ctx.is_authority = 1;
	ctx.is_in_session = 1;
	ctx.config.game_type = rules.game_type;
	ns::UdpSessionTransport wire(ns::UdpSessionTransport::Role::Host);
	auto &roster = ctx.np_protocol.connection_list;
	roster.push_back(make_seeded_conn(3, 1, &wire, ns::TransportMode::Client, player, true));
	roster[0].phase = inmatch::ConnectionPhase::InMatch;
	roster[0].reply.player_slot = 1;

	if (!expect(world.match.player(player) != nullptr &&
			!world.match.player(player)->spectator,
			"a joined player's roster row starts non-spectator"))
		return false;
	return expect(inmatch::Server_SetPlayerSpectator(ctx, roster[0], world, true) &&
			roster[0].link.spectator && world.match.player(player)->spectator,
			"the runtime spectator convert mirrors the latch onto the roster row");
}

// --------------------------------------------------------------------------
// The armory-reuse cooldown around C2S 0x2F.
// --------------------------------------------------------------------------

bool check_armory_reuse_cooldown() {
	w::World world;
	world.rules.mp_session = true;
	world.registry.configure_pool(0, 4);
	const w::EntityHandle player = w::spawn_remote_player(world, player_spawn(0, 0, 0));
	std::vector<inmatch::NapiNPConnection> roster;
	roster.push_back(make_seeded_conn(3, 1, nullptr, ns::TransportMode::Client, player, true));
	inmatch::NapiNPConnection &conn = roster.front();
	w::Entity *entity = world.registry.get(player);
	inmatch::GameConfig config;
	config.game_type = game_type::kTeamDeathmatch;

	LoadoutSubmit request;
	request.team = 1;
	request.player_class = 6;
	request.weapon_slot_index = 195;
	request.entries.push_back(LoadoutSubmitEntry{0x18, 0x03, 0xFF, 0xFF});
	const std::vector<uint8_t> submit = encode_loadout_submit(request);
	auto dispatch = [&](const std::vector<uint8_t> &body) {
		return inmatch::dispatch_session_replies(config, conn,
				{make_protocol_message(c2s::LOADOUT_SUBMIT, body)}, 100, roster, &world);
	};

	// (b) An alive, deployed player outside an armory zone with the latch clear
	// gets the current (empty) list and no state write.
	std::vector<ProtocolMessage> replies = dispatch(submit);
	if (!expect(replies.size() == 1 && replies[0].tag == s2c::WEAPON_LOADOUT &&
			conn.link.armory_reuse_seconds == 0 && entity->player_class != 6 &&
			!conn.reply.loadout_synced,
			"an alive player outside an armory zone is answered with its current list"))
		return false;
	// A dead player passes the armory-window gate: the grant seeds the cooldown.
	entity->flags |= w::kEntityFlagDead;
	replies = dispatch(submit);
	if (!expect(replies.size() == 1 && replies[0].tag == s2c::WEAPON_LOADOUT &&
			conn.link.armory_reuse_seconds == 30 && entity->player_class == 6 &&
			conn.reply.loadout_synced && !conn.link.preround_loadout_latch,
			"a dead player's submit is granted and seeds the 30 s cooldown"))
		return false;
	const std::vector<uint8_t> granted = conn.reply.last_loadout_reply;
	// (c) Inside the window the same current list comes back and nothing changes.
	entity->player_class = 8;
	replies = dispatch(submit);
	if (!expect(replies.size() == 1 && replies[0].payload == granted &&
			entity->player_class == 8 && conn.link.armory_reuse_seconds == 30,
			"a second nonzero-class submit inside the window is answered with the retained list"))
		return false;
	// The 1 Hz decrement rides tick_respawn_holds.
	{
		inmatch::NapiNPServerCtx ctx;
		ctx.world = &world;
		ctx.is_authority = 1;
		ctx.is_in_session = 1;
		ctx.config = config;
		ns::UdpSessionTransport transport(ns::UdpSessionTransport::Role::Host);
		ctx.np_protocol.connection_list.push_back(conn);
		ctx.np_protocol.connection_list.front().link.transport = &transport;
		ctx.np_protocol.connection_list.front().link.armory_reuse_seconds = -4;
		tick_to_periodic_second(ctx);
		if (!expect(ctx.np_protocol.connection_list.front().link.armory_reuse_seconds == 0,
				"a negative cooldown clamps to zero on the periodic second"))
			return false;
		ctx.np_protocol.connection_list.front().link.armory_reuse_seconds = 30;
		tick_to_periodic_second(ctx);
		if (!expect(ctx.np_protocol.connection_list.front().link.armory_reuse_seconds == 29,
				"the cooldown counts down once per periodic second"))
			return false;
	}
	// Expired: granted again, re-seeded.
	conn.link.armory_reuse_seconds = 0;
	replies = dispatch(submit);
	if (!expect(replies.size() == 1 && entity->player_class == 6 &&
			conn.link.armory_reuse_seconds == 30,
			"an expired cooldown admits the submit and re-arms it"))
		return false;
	// Pre-round: the window gate and the cooldown gate are both bypassed; the
	// latch suppresses the seed exactly once.
	world.preround_delay_seconds = 5;
	conn.link.preround_loadout_latch = true;
	conn.link.armory_reuse_seconds = 30;
	entity->player_class = 8;
	replies = dispatch(submit);
	if (!expect(replies.size() == 1 && entity->player_class == 6 &&
			conn.link.armory_reuse_seconds == 30 && !conn.link.preround_loadout_latch,
			"the pre-round latch admits the submit without re-seeding and clears itself"))
		return false;
	conn.link.armory_reuse_seconds = 0;
	entity->player_class = 8;
	replies = dispatch(submit);
	if (!expect(replies.size() == 1 && entity->player_class == 6 &&
			conn.link.armory_reuse_seconds == 30,
			"the next pre-round submit seeds the cooldown"))
		return false;
	world.preround_delay_seconds = 0;
	// Class 0 seeds too.
	conn.link.armory_reuse_seconds = 0;
	LoadoutSubmit class_zero = request;
	class_zero.player_class = 0;
	replies = dispatch(encode_loadout_submit(class_zero));
	if (!expect(replies.size() == 1 && entity->player_class == 0 &&
			conn.link.armory_reuse_seconds == 30,
			"a class-0 submit is accepted and seeds the cooldown"))
		return false;
	// An alive player inside an armory zone passes the window gate.
	entity->flags &= ~w::kEntityFlagDead;
	entity->flags |= w::kEntityFlagArmoryZone;
	conn.link.armory_reuse_seconds = 0;
	replies = dispatch(submit);
	if (!expect(replies.size() == 1 && entity->player_class == 6,
			"an alive player inside an armory zone is granted"))
		return false;
	// A spectator's submit returns with nothing: no reply, no cooldown (D-NET-678)
	// [orig: NapiNPServerMsg_HandlePlayerLoadout @0x5157EB..0x5157F2].
	conn.link.spectator = true;
	conn.link.armory_reuse_seconds = 0;
	replies = dispatch(submit);
	return expect(replies.empty() && conn.link.armory_reuse_seconds == 0,
			"a spectator submit draws nothing and seeds no cooldown");
}

// --------------------------------------------------------------------------
// The 744-tick priority-target sweep.
// --------------------------------------------------------------------------

// The S2C 0x68 rides each quartet unless the host's game.cfg allowcustomskins
// word is set, on a dedicated host as on a listen host; its cursor advances by
// 50 and wraps to 0 at the host's own frozen loaded-model list count (the
// count retail's misnamed viewport getter returns): with 305 frozen rows it
// asks 50, 100, ..., 300, then 0, 50; with none frozen every request wraps to
// 0; with allowcustomskins set no 0x68 goes at all.
// [orig: Server_SendLoadedModelPageRequest @0x511360 - the allowcustomskins test
//  @0x511363..0x51136A (Config_ParseSettingsLine @0x54F818 / @0x54F837), +50
//  @0x51137E, the `jb` against g_LoadedModelChallengeCount
//  (CEffectWorld_GetLoadedModelPage @0x5B1578) @0x511391, the wrap
//  @0x511393, the send @0x5113D4]
bool check_model_page_cursor_wraps_at_the_list_count() {
	const auto cursors = [](uint32_t list_count, int quartets, int32_t allow_custom_skins) {
		inmatch::NapiNPServerCtx ctx;
		inmatch::set_connection_mode(ctx, inmatch::ConnectionMode::HostOnly);
		inmatch::set_transport_mode(ctx, inmatch::SocketMode::Socketless);
		inmatch::GameConfig config;
		config.max_players = 4;
		config.allow_custom_skins = allow_custom_skins;
		inmatch::create_session(ctx, config, inmatch::SessionStartup{}, nullptr);
		auto heap = std::make_unique<w::World>();
		w::World &world = *heap;
		world.rules.mp_session = true;
		world.registry.configure_pool(0, 8);
		const w::EntityHandle player = w::spawn_remote_player(world, w::PlayerSpawn{});
		ctx.world = &world;
		ctx.loaded_model_list_count = list_count;
		ns::UdpSessionTransport transport(ns::UdpSessionTransport::Role::Host);
		ctx.np_protocol.connection_list.push_back(make_seeded_conn(
				7, inmatch::NapiNPConnection::kTypeServerSide, &transport,
				ns::TransportMode::Client, player, true));
		auto &peer = ctx.np_protocol.connection_list.back();
		peer.reply.player_slot = 1;
		world.match.upsert_player({player, 1, "Peer"});
		world.match.player(player)->play_ticks = inmatch::CONTROL_REQUEST_LIVE_GATE_TICKS;
		std::vector<uint32_t> out;
		for (int q = 0; q < quartets; ++q) {
			peer.reply.control_request_countdown = 0;
			peer.reply.charattr_unanswered_count = 0;
			peer.reply.time_sync_unanswered_count = 0;
			inmatch::Server_TickUpdate(ctx);
			for (const ns::Datagram &d : drain(transport))
				if (d.tag == s2c::LOADED_MODEL_PAGE_REQUEST && d.body.size() == 4)
					out.push_back(static_cast<uint32_t>(d.body[0]) |
							(static_cast<uint32_t>(d.body[1]) << 8) |
							(static_cast<uint32_t>(d.body[2]) << 16) |
							(static_cast<uint32_t>(d.body[3]) << 24));
		}
		return out;
	};
	bool ok = expect(cursors(305, 8, 0) ==
					std::vector<uint32_t>({50, 100, 150, 200, 250, 300, 0, 50}),
			"a dedicated host's 0x68 cursor wraps to 0 at the 305-row list");
	ok = expect(cursors(0, 2, 0) == std::vector<uint32_t>({0, 0}),
			"with no list frozen every 0x68 asks row 0") && ok;
	ok = expect(cursors(305, 2, 1).empty(),
			"allowcustomskins set sends no 0x68") && ok;
	return ok;
}

// A remote slot whose spectator latch is set takes no control quartet (and
// draws no silence punt), mature or not; its latch cleared, the next walk sends
// the 0x39 / 0x42 / 0x43 / 0x68.
// [orig: Server_UpdateAllActivePlayerSlots - the spectator latch +0x188D7
//  @0x5189CE ahead of the live age @0x5189DA and the punts @0x5189EF..0x518A3A]
bool check_quartet_skips_a_spectator_slot() {
	inmatch::NapiNPServerCtx ctx;
	inmatch::set_connection_mode(ctx, inmatch::ConnectionMode::HostOnly);
	inmatch::set_transport_mode(ctx, inmatch::SocketMode::Socketless);
	inmatch::GameConfig config;
	config.max_players = 4;
	inmatch::create_session(ctx, config, inmatch::SessionStartup{}, nullptr);
	auto heap = std::make_unique<w::World>();
	w::World &world = *heap;
	world.rules.mp_session = true;
	world.registry.configure_pool(0, 8);
	const w::EntityHandle player = w::spawn_remote_player(world, w::PlayerSpawn{});
	ctx.world = &world;
	ns::UdpSessionTransport transport(ns::UdpSessionTransport::Role::Host);
	ctx.np_protocol.connection_list.push_back(make_seeded_conn(
			7, inmatch::NapiNPConnection::kTypeServerSide, &transport,
			ns::TransportMode::Client, player, true));
	auto &peer = ctx.np_protocol.connection_list.back();
	peer.reply.player_slot = 1;
	peer.link.spectator = true;
	world.match.upsert_player({player, 1, "Peer"});
	world.match.player(player)->play_ticks = inmatch::CONTROL_REQUEST_LIVE_GATE_TICKS;
	peer.reply.control_request_countdown = 0;
	peer.reply.charattr_unanswered_count = 8; // a silence the punt would read
	const auto quartet_tags = [&]() {
		int n = 0;
		for (const ns::Datagram &d : drain(transport))
			if (d.tag == s2c::CHARATTR_CRC_CHALLENGE || d.tag == s2c::CHARATTR_DISABLED_PROPERTIES ||
					d.tag == s2c::TIME_SYNC_PING || d.tag == s2c::LOADED_MODEL_PAGE_REQUEST)
				++n;
		return n;
	};
	inmatch::Server_TickUpdate(ctx);
	if (!expect(quartet_tags() == 0 && !peer.host_disconnect_sent &&
	                    peer.reply.control_request_countdown == 0,
	            "a spectator slot takes no quartet and no silence punt"))
		return false;
	peer.link.spectator = false;
	peer.reply.charattr_unanswered_count = 0;
	inmatch::Server_TickUpdate(ctx);
	return expect(quartet_tags() == 4, "the latch cleared, the next walk sends the quartet");
}

// A silence punt does not end the slot's visit: the countdown runs in the same
// visit (one punted visit leaves it one lower), and at its zero the quartet's
// state moves (the counts, the time-sync sequence, the model cursor) while the
// connection, its description gone, queues nothing more: the wire carries the
// description alone.
// [orig: Server_UpdateAllActivePlayerSlots @0x518820 - the punts
//  @0x5189EF..0x518A3A falling to the countdown @0x518A42..0x518A5D;
//  CNapiNPConnection_QueueMessage @0x628640, the `desc_sent` drop @0x62864D]
bool check_silence_punt_falls_through_to_the_countdown() {
	const auto ctx_owner = std::make_unique<inmatch::NapiNPServerCtx>();
	inmatch::NapiNPServerCtx &ctx = *ctx_owner;
	inmatch::set_connection_mode(ctx, inmatch::ConnectionMode::HostOnly);
	inmatch::set_transport_mode(ctx, inmatch::SocketMode::Socketless);
	inmatch::GameConfig config;
	config.max_players = 4;
	inmatch::create_session(ctx, config, inmatch::SessionStartup{}, nullptr);
	ctx.loaded_model_list_count = 305;
	auto heap = std::make_unique<w::World>();
	w::World &world = *heap;
	world.rules.mp_session = true;
	world.registry.configure_pool(0, 8);
	const w::EntityHandle pa = w::spawn_remote_player(world, w::PlayerSpawn{});
	const w::EntityHandle pb = w::spawn_remote_player(world, w::PlayerSpawn{});
	ctx.world = &world;
	const auto ta_owner = std::make_unique<ns::UdpSessionTransport>(ns::UdpSessionTransport::Role::Host);
	const auto tb_owner = std::make_unique<ns::UdpSessionTransport>(ns::UdpSessionTransport::Role::Host);
	ns::UdpSessionTransport &ta = *ta_owner;
	ns::UdpSessionTransport &tb = *tb_owner;
	ctx.np_protocol.connection_list.push_back(make_seeded_conn(
			7, inmatch::NapiNPConnection::kTypeServerSide, &ta, ns::TransportMode::Client, pa, true));
	ctx.np_protocol.connection_list.back().reply.player_slot = 1;
	ctx.np_protocol.connection_list.push_back(make_seeded_conn(
			8, inmatch::NapiNPConnection::kTypeServerSide, &tb, ns::TransportMode::Client, pb, true));
	ctx.np_protocol.connection_list.back().reply.player_slot = 2;
	inmatch::NapiNPConnection &a = ctx.np_protocol.connection_list[0];
	inmatch::NapiNPConnection &b = ctx.np_protocol.connection_list[1];
	world.match.upsert_player({pa, 1, "A"});
	world.match.upsert_player({pb, 2, "B"});
	for (const w::EntityHandle h : {pa, pb})
		world.match.player(h)->play_ticks = inmatch::CONTROL_REQUEST_LIVE_GATE_TICKS;
	// Peer A: the character-attribute silence (the 16 arm), countdown 2.
	a.reply.charattr_unanswered_count = 8;
	a.reply.control_request_countdown = 2;
	// Peer B: answered challenges, the time-sync silence (the 24 arm), and a
	// countdown that reaches zero in the punted visit.
	b.reply.time_sync_unanswered_count = 8;
	b.reply.control_request_countdown = 1;
	b.reply.control_challenge_seed = 0x1234;
	const uint32_t sequence = b.reply.time_sync_sequence;
	const auto tags = [](ns::UdpSessionTransport &t) {
		std::vector<uint8_t> out;
		for (const ns::Datagram &d : drain(t)) out.push_back(d.tag);
		return out;
	};
	inmatch::Server_TickUpdate(ctx);
	const std::vector<uint8_t> a_tags = tags(ta);
	const std::vector<uint8_t> b_tags = tags(tb);
	const auto punted = [](const std::vector<uint8_t> &t) {
		return std::count(t.begin(), t.end(), hightag::DESCRIPTION_PACKET) == 1 &&
				std::count(t.begin(), t.end(), s2c::CHARATTR_CRC_CHALLENGE) == 0 &&
				std::count(t.begin(), t.end(), s2c::CHARATTR_DISABLED_PROPERTIES) == 0 &&
				std::count(t.begin(), t.end(), s2c::TIME_SYNC_PING) == 0 &&
				std::count(t.begin(), t.end(), s2c::LOADED_MODEL_PAGE_REQUEST) == 0;
	};
	if (!expect(a.host_disconnect_sent && a.host_disconnect_mismatch_type == 16 &&
	                    punted(a_tags) && a.reply.control_request_countdown == 1,
	            "the 16 punt's visit runs the countdown on (2 -> 1)"))
		return false;
	return expect(b.host_disconnect_sent && b.host_disconnect_mismatch_type == 24 &&
	                      punted(b_tags) &&
	                      b.reply.control_request_countdown ==
	                              inmatch::CONTROL_REQUEST_PERIOD_TICKS &&
	                      b.reply.charattr_unanswered_count == 1 &&
	                      b.reply.time_sync_unanswered_count == 9 &&
	                      b.reply.time_sync_sequence == sequence + 1u &&
	                      b.reply.loaded_model_page_cursor == 50u,
	              "the 24 punt's visit reaches the zero: the quartet's state moves, "
	              "nothing queues after the description");
}

// The integrity family spends one PRNG_Next16 draw per request it sends, the
// slot's word filled and zeroed again, so every request carries 0: a 0x31
// boundary draws once per resolvable ADM row (the unresolved peer sends and
// draws nothing; a resolved row with no round type sends 0xFF, the low byte
// of adm+4's default -1, and draws), the next boundary's 0x30 to all three
// peers draws three times.
// [orig: Server_BroadcastWeaponSlotStateToPlayers @0x508540 - the 0x30 draw
//  @0x5085A6..0x5085B9, the 0x31's ADM row @0x508605..0x508611, its draw
//  @0x508613..0x508626 and byte @0x50863E; PRNG_Next16 @0x6130A0]
bool check_integrity_family_draws_once_per_send() {
	inmatch::NapiNPServerCtx ctx;
	inmatch::set_connection_mode(ctx, inmatch::ConnectionMode::HostOnly);
	inmatch::set_transport_mode(ctx, inmatch::SocketMode::Socketless);
	inmatch::GameConfig config;
	config.max_players = 4;
	inmatch::create_session(ctx, config, inmatch::SessionStartup{}, nullptr);
	auto heap = std::make_unique<w::World>();
	w::World &world = *heap;
	world.rules.mp_session = true;
	world.registry.configure_pool(0, 8);
	world.tables.weapons.entries.resize(0x21);
	world.tables.weapons.entries[0x20].valid = true;
	world.tables.weapons.entries[0x20].ammo_index = 0x1A;
	// A row with no round type keeps adm+4's default -1
	// [orig: AdmDef_InitEntryDefaults @0x53FF16 / @0x53FF58].
	world.tables.weapons.entries[0x1F].valid = true;
	world.tables.weapons.entries[0x1F].ammo_index = -1;
	ctx.world = &world;
	ns::UdpSessionTransport ta(ns::UdpSessionTransport::Role::Host);
	ns::UdpSessionTransport tb(ns::UdpSessionTransport::Role::Host);
	ns::UdpSessionTransport tc(ns::UdpSessionTransport::Role::Host);
	const w::EntityHandle pa = w::spawn_remote_player(world, w::PlayerSpawn{});
	const w::EntityHandle pb = w::spawn_remote_player(world, w::PlayerSpawn{});
	const w::EntityHandle pc = w::spawn_remote_player(world, w::PlayerSpawn{});
	for (const w::EntityHandle h : {pa, pb, pc}) {
		// Hold the bodies so only the family draws from the stream
		// [orig: Entity_UpdateAllEntities @0x4C2460..0x4C2474 skips a held +0x1C4].
		world.registry.get(h)->motor_suspended = true;
	}
	world.registry.get(pa)->equipped_adm_index = 0x20;
	world.registry.get(pb)->equipped_adm_index = 0xFF; // no ADM row
	world.registry.get(pc)->equipped_adm_index = 0x1F; // a row, no round type
	ctx.np_protocol.connection_list.push_back(make_seeded_conn(
			7, inmatch::NapiNPConnection::kTypeServerSide, &ta, ns::TransportMode::Client, pa, true));
	ctx.np_protocol.connection_list.back().reply.player_slot = 1;
	ctx.np_protocol.connection_list.push_back(make_seeded_conn(
			8, inmatch::NapiNPConnection::kTypeServerSide, &tb, ns::TransportMode::Client, pb, true));
	ctx.np_protocol.connection_list.back().reply.player_slot = 2;
	ctx.np_protocol.connection_list.push_back(make_seeded_conn(
			9, inmatch::NapiNPConnection::kTypeServerSide, &tc, ns::TransportMode::Client, pc, true));
	ctx.np_protocol.connection_list.back().reply.player_slot = 3;
	world.match.upsert_player({pa, 1, "A"});
	world.match.upsert_player({pb, 1, "B"});
	world.match.upsert_player({pc, 1, "C"});
	world.prng16_state = 1;
	ctx.integrity_entity_family_next = false;
	const auto requests = [](ns::UdpSessionTransport &t, uint8_t tag) {
		std::vector<std::vector<uint8_t>> out;
		for (const ns::Datagram &d : drain(t))
			if (d.tag == tag) out.push_back(d.body);
		return out;
	};
	// PRNG_Next16 from state 1: s = rol4(s + rol11(s)) ^ 1.
	const auto state_after = [](int draws) {
		uint32_t s = 1;
		for (int i = 0; i < draws; ++i) {
			const uint32_t sum = s + ((s << 11) | (s >> 21));
			s = ((sum << 4) | (sum >> 28)) ^ 1u;
		}
		return s;
	};
	ctx.scoreboard_broadcast_timer = 0x136u;
	inmatch::Server_TickUpdate(ctx);
	const auto a31 = requests(ta, s2c::LOADOUT_CRC_REQ);
	const auto b31 = requests(tb, s2c::LOADOUT_CRC_REQ);
	const auto c31 = requests(tc, s2c::LOADOUT_CRC_REQ);
	if (!expect(a31.size() == 1 && a31[0] == std::vector<uint8_t>({0x1A, 0x00, 0x00}) &&
	                    b31.empty() && world.prng16_state == state_after(2),
	            "a 0x31 boundary draws once per resolvable row and sends 0"))
		return false;
	// The row resolves, so the request goes with the low byte of -1
	// [orig: @0x508611, `mov ecx, [edi+4]` @0x50863E].
	if (!expect(c31.size() == 1 && c31[0] == std::vector<uint8_t>({0xFF, 0x00, 0x00}),
	            "a resolved row with no round type sends 0xFF and draws"))
		return false;
	ctx.scoreboard_broadcast_timer = 0x136u;
	inmatch::Server_TickUpdate(ctx);
	const auto a30 = requests(ta, s2c::ENTITY_CHECKSUM_REQ);
	const auto b30 = requests(tb, s2c::ENTITY_CHECKSUM_REQ);
	const auto c30 = requests(tc, s2c::ENTITY_CHECKSUM_REQ);
	return expect(a30.size() == 1 && b30.size() == 1 && c30.size() == 1 &&
	                      a30[0] == std::vector<uint8_t>({0xFF, 0x00, 0x00}) &&
	                      b30[0] == a30[0] && c30[0] == a30[0] &&
	                      world.prng16_state == state_after(5),
	              "the next boundary's 0x30 to all three peers draws three times");
}

// D-NET-NEW-L3-5: the per-slot 0x0A runs in or out of a session; only the 0x57
// beside it is session-gated. A closed session's in-match peer still takes its
// 0x0A and no 0x57; an open one takes both.
// [orig: Server_TickUpdate - is_in_session @0x51E211..0x51E217 lands on the
//  per-slot loop @0x51E385, the 0x0A call @0x51E3E4, the 0x57's session test
//  @0x51E3EC]
bool check_entity_state_fan_runs_out_of_session() {
	inmatch::NapiNPServerCtx ctx;
	inmatch::set_connection_mode(ctx, inmatch::ConnectionMode::HostOnly);
	inmatch::set_transport_mode(ctx, inmatch::SocketMode::Socketless);
	inmatch::GameConfig config;
	config.max_players = 4;
	inmatch::create_session(ctx, config, inmatch::SessionStartup{}, nullptr);
	auto heap = std::make_unique<w::World>();
	w::World &world = *heap;
	world.rules.mp_session = true;
	world.registry.configure_pool(0, 8);
	const w::EntityHandle player = w::spawn_remote_player(world, w::PlayerSpawn{});
	ctx.world = &world;
	ns::UdpSessionTransport transport(ns::UdpSessionTransport::Role::Host);
	ctx.np_protocol.connection_list.push_back(make_seeded_conn(
			7, inmatch::NapiNPConnection::kTypeServerSide, &transport,
			ns::TransportMode::Client, player, true));
	ctx.np_protocol.connection_list.back().reply.player_slot = 1;
	world.match.upsert_player({player, 1, "Peer"});
	const auto tags = [&]() {
		std::vector<uint8_t> out;
		for (const ns::Datagram &d : drain(transport)) out.push_back(d.tag);
		return out;
	};
	const auto has = [](const std::vector<uint8_t> &t, uint8_t tag) {
		return std::find(t.begin(), t.end(), tag) != t.end();
	};
	ctx.is_in_session = 0;
	inmatch::Server_TickUpdate(ctx);
	const std::vector<uint8_t> closed = tags();
	if (!expect(has(closed, s2c::PER_FRAME_UPDATE) && !has(closed, s2c::RTT_ECHO),
	            "a closed session's peer takes its 0x0A and no 0x57"))
		return false;
	ctx.is_in_session = 1;
	ctx.np_protocol.connection_list.back().reply.rtt_request_countdown = 0;
	inmatch::Server_TickUpdate(ctx);
	const std::vector<uint8_t> open = tags();
	return expect(has(open, s2c::PER_FRAME_UPDATE) && has(open, s2c::RTT_ECHO),
	              "an open session's peer takes its 0x0A and the 0x57");
}

// D-NET-441: a joiner loading between its add and its join (its slot out of
// state 6) takes no proximity-pass service (its event-25 tick here) and no
// place in the capture census, while a joined slot in the same zone takes
// both; once it joins, its next pass counts it.
// [orig: Server_UpdateCaptureZoneProximity - state 6 @0x50872A, the clear
//  @0x50872E, `jnz` @0x508735; CaptureZone_CalculateControlDelta - state 6
//  @0x501188, the presence compare @0x50120D..0x501248; Server_OnPlayerJoin's
//  state 6 @0x51A712]
bool check_a_loading_joiner_takes_no_proximity_or_census_place() {
	inmatch::NapiNPServerCtx ctx;
	inmatch::set_connection_mode(ctx, inmatch::ConnectionMode::HostOnly);
	inmatch::set_transport_mode(ctx, inmatch::SocketMode::Socketless);
	inmatch::GameConfig config;
	config.max_players = 4;
	config.game_type = game_type::kTeamDeathmatch;
	inmatch::create_session(ctx, config, inmatch::SessionStartup{}, nullptr);
	auto heap = std::make_unique<w::World>();
	w::World &world = *heap;
	world.rules.mp_session = true;
	world.registry.configure_pool(0, 8);
	world.registry.configure_pool(1, 4);
	w::MatchRules rules;
	rules.game_type = game_type::kTeamDeathmatch;
	world.match.configure(rules);
	ctx.world = &world;
	w::Entity zone;
	zone.kind = w::EntityKind::Item;
	zone.is_capture_trigger = true;
	zone.zone_number = 1;
	zone.zone_radius = 20;
	zone.team = 1;
	zone.health = 1;
	zone.alive = true;
	world.registry.spawn(1, zone);
	const w::EntityHandle joined = w::spawn_remote_player(world, player_spawn(0, 0, 0, 1));
	const w::EntityHandle loading = w::spawn_remote_player(world, player_spawn(0, 0, 0, 1));
	ns::UdpSessionTransport tj(ns::UdpSessionTransport::Role::Host);
	ns::UdpSessionTransport tl(ns::UdpSessionTransport::Role::Host);
	ctx.np_protocol.connection_list.push_back(make_seeded_conn(
			7, inmatch::NapiNPConnection::kTypeServerSide, &tj, ns::TransportMode::Client, joined, true));
	ctx.np_protocol.connection_list.back().reply.player_slot = 1;
	ctx.np_protocol.connection_list.push_back(make_seeded_conn(
			8, inmatch::NapiNPConnection::kTypeServerSide, &tl, ns::TransportMode::Client, loading, false));
	inmatch::NapiNPConnection &joiner = ctx.np_protocol.connection_list.back();
	joiner.reply.player_slot = 2;
	joiner.phase = inmatch::ConnectionPhase::PlayerAdded;
	joiner.slot_state = inmatch::PlayerSlotState::AwaitJoin; // its world stream ended, no 0x0B yet
	world.match.upsert_player({joined, 1, "Joined"});
	world.match.upsert_player({loading, 2, "Loading"});
	for (const w::EntityHandle h : {joined, loading}) world.match.set_player_slot_team(h, 1);
	const auto friendlies = [&]() {
		int out = -1;
		for (const ns::Datagram &d : drain(tj))
			if (d.tag == s2c::ZONE_TIMER_VALUE && d.body.size() == 15) out = d.body[13];
		(void)drain(tl);
		return out;
	};
	// Two passes: the session's first tick (the zero-armed countdown fires at
	// once), then a periodic second later.
	inmatch::Server_TickUpdate(ctx);
	(void)friendlies();
	tick_to_periodic_second(ctx);
	const w::MatchPlayer *jr = world.match.player(joined);
	const w::MatchPlayer *lr = world.match.player(loading);
	if (!expect(jr->periodic_score_ticks == 2, "the joined slot takes both passes' event-25 ticks"))
		return false;
	if (!expect(lr->periodic_score_ticks == 0, "the loading joiner takes no event-25 tick"))
		return false;
	if (!expect(friendlies() == 1, "the capture census counts the joined slot alone"))
		return false;
	// The join: state 6, and its initial-state stream completes.
	joiner.slot_state = inmatch::PlayerSlotState::Joined;
	joiner.phase = inmatch::ConnectionPhase::InMatch;
	joiner.burst.spawned = true;
	tick_to_periodic_second(ctx);
	if (!expect(lr->periodic_score_ticks == 1, "joined, its next pass counts it"))
		return false;
	return expect(friendlies() == 2, "joined, the census counts both");
}

// D-NET-593: a script team write moves a body's team byte alone; the readers
// of the slot's own team byte keep the slot's. The visible-players snapshot
// lists the requester's slot-team mates, the 0x32 join line carries the slot
// team, and the status page colours by it.
// [orig: NetPacket_SerializeVisiblePlayersSnapshot @0x50636B / @0x506422..0x506429;
//  Server_PlayerAdd @0x51D25C; Server_DrawStatusScreen @0x50A565]
bool check_slot_team_readers_ignore_a_body_team_write() {
	inmatch::NapiNPServerCtx ctx;
	inmatch::set_connection_mode(ctx, inmatch::ConnectionMode::HostOnly);
	inmatch::set_transport_mode(ctx, inmatch::SocketMode::Socketless);
	inmatch::GameConfig config;
	config.max_players = 4;
	config.game_type = game_type::kTeamDeathmatch;
	inmatch::create_session(ctx, config, inmatch::SessionStartup{}, nullptr);
	auto heap = std::make_unique<w::World>();
	w::World &world = *heap;
	world.rules.mp_session = true;
	world.registry.configure_pool(0, 8);
	ctx.world = &world;
	std::array<ns::UdpSessionTransport, 3> wires{
			ns::UdpSessionTransport(ns::UdpSessionTransport::Role::Host),
			ns::UdpSessionTransport(ns::UdpSessionTransport::Role::Host),
			ns::UdpSessionTransport(ns::UdpSessionTransport::Role::Host)};
	const std::array<uint8_t, 3> slot_teams{1, 1, 2};
	for (int i = 0; i < 3; ++i) {
		const w::EntityHandle h =
				w::spawn_remote_player(world, player_spawn(10.0f * float(i), 0, 0, slot_teams[i]));
		ctx.np_protocol.connection_list.push_back(make_seeded_conn(
				static_cast<uint32_t>(7 + i), inmatch::NapiNPConnection::kTypeServerSide,
				&wires[i], ns::TransportMode::Client, h, true));
		inmatch::NapiNPConnection &c = ctx.np_protocol.connection_list.back();
		c.reply.player_slot = static_cast<uint32_t>(i + 1);
		c.reply.player_name = std::string(1, static_cast<char>('A' + i));
		c.assigned_team = slot_teams[i];
		c.assigned_team_valid = true;
	}
	inmatch::NapiNPConnection &a = ctx.np_protocol.connection_list[0];
	world.registry.get(a.link.owned_entity)->team = 2; // the script's write to the body
	VisiblePlayers seen;
	const std::vector<uint8_t> snapshot = inmatch::build_visible_players_snapshot(
			a, ctx.np_protocol.connection_list, game_type::kTeamDeathmatch, false);
	decode_visible_players(snapshot.data(), snapshot.size(), seen);
	if (!expect(seen.entries.size() == 2 && seen.entries[0].slot == 1 && seen.entries[1].slot == 2,
	            "the snapshot lists the requester's slot-team mates"))
		return false;
	for (auto &wire : wires) (void)drain(wire);
	inmatch::broadcast_player_joined_text(ctx.np_protocol.connection_list, a);
	const std::vector<uint8_t> line = {1, 'A', 0, 1};
	bool carried = false;
	for (const ns::Datagram &d : drain(wires[1]))
		carried = carried || (d.tag == s2c::FORMATTED_GAME_TEXT && d.body == line);
	if (!expect(carried, "the join line carries the slot team"))
		return false;
	hud::ServerStatusPageState page;
	inmatch::fill_server_status_page(page, ctx, &world);
	return expect(page.slots.size() > 1 && page.slots[1].team == 1,
	              "the status page colours by the slot team");
}

// The host's CNetQuality sample runs in the main frame ahead of the server
// tick, so ahead of the receive pump: a ping the pump stores this frame
// reaches the next sample, not this one.
// [orig: Game_ProcessMainFrame - CNetQuality_UpdateMetrics @0x52658B ahead of
//  the Server_TickUpdate call @0x5266B6; Server_TickUpdate's receive pump
//  @0x51D895]
bool check_host_quality_samples_ahead_of_the_receive_pump() {
	const auto sample = [](uint32_t ring_before, bool pump_writes) {
		inmatch::NapiNPServerCtx ctx;
		inmatch::set_connection_mode(ctx, inmatch::ConnectionMode::HostOnly);
		inmatch::set_transport_mode(ctx, inmatch::SocketMode::Socketless);
		inmatch::GameConfig config;
		config.max_players = 4;
		inmatch::create_session(ctx, config, inmatch::SessionStartup{}, nullptr);
		auto heap = std::make_unique<w::World>();
		w::World &world = *heap;
		world.rules.mp_session = true;
		world.registry.configure_pool(0, 8);
		ctx.world = &world;
		ctx.stats_avg_fps = 62;
		const w::EntityHandle player = w::spawn_remote_player(world, w::PlayerSpawn{});
		ns::UdpSessionTransport transport(ns::UdpSessionTransport::Role::Host);
		ctx.np_protocol.connection_list.push_back(make_seeded_conn(
				7, inmatch::NapiNPConnection::kTypeServerSide, &transport,
				ns::TransportMode::Client, player, true));
		inmatch::NapiNPConnection &peer = ctx.np_protocol.connection_list.back();
		peer.reply.player_slot = 1;
		world.match.upsert_player({player, 1, "Peer"});
		world.match.player(player)->play_ticks = inmatch::CONTROL_REQUEST_LIVE_GATE_TICKS;
		for (uint32_t &r : peer.reply.rtt_ring) r = ring_before;
		ctx.net_quality_sample_countdown = 1; // this frame samples
		if (pump_writes)
			ctx.receive_pump = [&peer]() {
				for (uint32_t &r : peer.reply.rtt_ring) r = 250;
			};
		inmatch::Server_TickUpdate(ctx);
		return ctx.host_network_quality;
	};
	const uint8_t before = sample(0, false);
	const uint8_t pumped = sample(0, true);
	const uint8_t slow = sample(250, false);
	if (!expect(before != slow, "a 250 ms ring samples apart from a zero one"))
		return false;
	return expect(pumped == before, "the pump's ping waits for the next sample");
}

// D-NET-NEW-L3-4: a designator round (a DesignateTarget ammo) flies nothing:
// the authority marks its firer's mode-1 designation at the spawn point with
// the weapon's life (+0x458) and radius (+0x454), and a fresh mode-1 row sends
// S2C 0x1E [0x30][the firer's pool-0 index][0xFF 0xFF][0 0 0 0] to every
// in-match slot on the firer's team; the owner's next mark refreshes its row
// with no second notice.
// [orig: RoundData_SpawnRound @0x4EC249..0x4EC270; EntityTracker_RegisterOrUpdate
//  @0x511580 - the notice test @0x5115D4..0x5115DC, the body @0x511608..0x511632, the send
//  @0x511645, the stores @0x511656..0x511676]
bool check_designator_round_marks_and_notifies_the_team() {
	inmatch::NapiNPServerCtx ctx;
	inmatch::set_connection_mode(ctx, inmatch::ConnectionMode::HostOnly);
	inmatch::set_transport_mode(ctx, inmatch::SocketMode::Socketless);
	inmatch::GameConfig config;
	config.max_players = 4;
	config.game_type = game_type::kTeamDeathmatch;
	inmatch::create_session(ctx, config, inmatch::SessionStartup{}, nullptr);
	auto heap = std::make_unique<w::World>();
	w::World &world = *heap;
	world.rules.mp_session = true;
	world.rules.projectile_authority = true;
	world.registry.configure_pool(0, 8);
	ctx.world = &world;
	world.tables.ammo.entries.resize(1);
	world.tables.ammo.entries[0].valid = true;
	world.tables.ammo.entries[0].flags = w::kAmmoFlagDesignateTarget;
	world.tables.weapons.entries.resize(0x21);
	world.tables.weapons.entries[0x20].valid = true;
	world.tables.weapons.entries[0x20].designation_ticks = 30 * 62;
	world.tables.weapons.entries[0x20].hud_splash_radius = 10;
	std::array<ns::UdpSessionTransport, 3> wires{
			ns::UdpSessionTransport(ns::UdpSessionTransport::Role::Host),
			ns::UdpSessionTransport(ns::UdpSessionTransport::Role::Host),
			ns::UdpSessionTransport(ns::UdpSessionTransport::Role::Host)};
	const std::array<uint8_t, 3> teams{1, 1, 2};
	std::array<w::EntityHandle, 3> bodies{};
	for (int i = 0; i < 3; ++i) {
		bodies[i] = w::spawn_remote_player(world, player_spawn(10.0f * float(i), 0, 0, teams[i]));
		ctx.np_protocol.connection_list.push_back(make_seeded_conn(
				static_cast<uint32_t>(7 + i), inmatch::NapiNPConnection::kTypeServerSide,
				&wires[i], ns::TransportMode::Client, bodies[i], true));
		inmatch::NapiNPConnection &c = ctx.np_protocol.connection_list.back();
		c.reply.player_slot = static_cast<uint32_t>(i + 1);
		c.assigned_team = teams[i];
		c.assigned_team_valid = true;
	}
	w::RoundSpawnParams shot;
	shot.owner = bodies[0];
	shot.ammo_index = 0;
	shot.adm_index = 0x20;
	shot.origin = {100.0f, 200.0f, 5.0f};
	if (!expect(world.round_sim.spawn(world, shot, w::RoundConsequenceMode::Authoritative) == -1 &&
	                    world.out.designation_marks.size() == 1,
	            "a designator round flies nothing and leaves one mark"))
		return false;
	for (auto &wire : wires) (void)drain(wire);
	inmatch::Server_TickUpdate(ctx); // the tick head routes the marks
	const std::vector<uint8_t> notice = {0x30, static_cast<uint8_t>(bodies[0].slot()),
			0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00};
	const auto notices = [&](ns::UdpSessionTransport &wire) {
		int n = 0;
		for (const ns::Datagram &d : drain(wire))
			if (d.tag == s2c::GAME_EVENT && d.body == notice) ++n;
		return n;
	};
	if (!expect(notices(wires[0]) == 1 && notices(wires[1]) == 1 && notices(wires[2]) == 0,
	            "the fresh mode-1 row notifies the firer's team alone"))
		return false;
	const inmatch::ServerDesignation *row = nullptr;
	for (const inmatch::ServerDesignation &r : ctx.designations)
		if (r.owner == bodies[0]) row = &r;
	if (!expect(row != nullptr && row->mode == 1 && row->point[0] == 100 * 65536 &&
	                    row->point[1] == 200 * 65536 && row->point[2] == 5 * 65536 &&
	                    row->radius_q16 == 10 * 65536 && row->remaining_ticks == 30 * 62 - 1,
	            "the row holds the point, the radius and the life (one tick aged)"))
		return false;
	(void)world.round_sim.spawn(world, shot, w::RoundConsequenceMode::Authoritative);
	inmatch::Server_TickUpdate(ctx);
	return expect(notices(wires[0]) == 0 && notices(wires[1]) == 0 &&
	                      row->remaining_ticks == 30 * 62 - 1,
	              "the owner's next mark refreshes its row with no second notice");
}

// The 1 Hz capture pass sends each zone's 0x6F to every in-match slot on
// every pass, once, whether or not its record changed and whatever the
// recipient's state; the 0x6E goes, once, to a slot whose body's dead bit or
// hold is set, and a Health-0 body with its dead bit clear (a death its edge
// has not latched yet) gets none.
// [orig: Server_UpdateCaptureZoneEntities @0x519690 - send_mask 0x80 @0x5197B1,
//  userParam 1 @0x5197D9; Server_TickUpdate - `test [eax+24h], 2` @0x51E083, the
//  hold @0x51E089, userParam 1 @0x51E0C4]
bool check_zone_timer_every_pass_and_wave_status_by_the_dead_bit() {
	inmatch::NapiNPServerCtx ctx;
	ctx.is_authority = 1;
	ctx.is_in_session = 1;
	ctx.config.game_type = game_type::kTeamDeathmatch;
	auto heap = std::make_unique<w::World>();
	w::World &world = *heap;
	world.rules.mp_session = true;
	ctx.world = &world;
	world.registry.configure_pool(0, 4);
	world.registry.configure_pool(1, 4);
	w::MatchRules rules;
	rules.game_type = ctx.config.game_type;
	world.match.configure(rules);
	w::Entity zone;
	zone.kind = w::EntityKind::Item;
	zone.is_capture_trigger = true;
	zone.zone_number = 1;
	zone.zone_radius = 20;
	zone.team = 1;
	zone.health = 1;
	zone.alive = true;
	const w::EntityHandle zone_handle = world.registry.spawn(1, zone);
	world.zones.build_chain_from_mission();
	w::Entity body;
	body.kind = w::EntityKind::Organic;
	body.player_class = 8;
	body.team = 1;
	body.health = 150;
	body.alive = true;
	body.position = {500.0f, 0.0f, 0.0f};
	const w::EntityHandle player = world.registry.spawn(0, body);
	world.match.upsert_player({player, 0, "Blue", {}});
	ns::UdpSessionTransport transport(ns::UdpSessionTransport::Role::Host);
	inmatch::NapiNPConnection conn;
	conn.type = 1;
	conn.phase = inmatch::ConnectionPhase::InMatch;
	conn.burst.spawned = true;
	conn.link.mode = ns::TransportMode::Client;
	conn.link.transport = &transport;
	conn.link.owned_entity = player;
	ctx.np_protocol.connection_list.push_back(std::move(conn));

	struct Pass {
		int zone_records = 0;
		bool zone_reliable = false;
		int wave_status = 0;
		bool wave_reliable = false;
	};
	const auto next_second = [&]() {
		do {
			inmatch::Server_TickUpdate(ctx);
		} while (!world.match.periodic_second());
		Pass out;
		ns::Datagram d;
		while (transport.pop_outbound(d)) {
			if (d.tag == s2c::ZONE_TIMER_VALUE && d.body.size() >= 2 &&
					(d.body[0] | (d.body[1] << 8)) == zone_handle.packed) {
				++out.zone_records;
				out.zone_reliable = out.zone_reliable || d.reliable;
			}
			if (d.tag == s2c::SPAWN_WAVE_STATUS) {
				++out.wave_status;
				out.wave_reliable = out.wave_reliable || d.reliable;
			}
		}
		return out;
	};
	(void)next_second();
	const Pass unchanged = next_second();
	bool ok = expect(unchanged.zone_records == 1 && !unchanged.zone_reliable &&
					unchanged.wave_status == 0,
			"an unchanged zone's 0x6F reaches a live slot again, sent once");
	w::Entity *e = world.registry.get(player);
	e->health = 0;
	const Pass unlatched = next_second();
	ok = expect(unlatched.zone_records == 1 && unlatched.wave_status == 0,
			"a Health-0 body with its dead bit clear draws no 0x6E") && ok;
	e->health = 150;
	e->engine_flags |= w::kEntityFlagDead;
	const Pass dead = next_second();
	ok = expect(dead.wave_status == 1 && !dead.wave_reliable,
			"the dead bit draws the one-send 0x6E") && ok;
	return ok;
}

bool check_priority_target_sweep() {
	w::World world;
	world.registry.configure_pool(0, 4);
	world.registry.configure_pool(1, 4);
	world.registry.configure_pool(2, 4);
	auto mark = [&](int pool) {
		w::Entity e;
		e.kind = pool == 0 ? w::EntityKind::Organic : w::EntityKind::Item;
		e.flags = w::kEntityFlagPriorityTarget;
		e.engine_flags = w::kEntityFlagPriorityTarget;
		return world.registry.spawn(pool, e);
	};
	const w::EntityHandle p0 = mark(0);
	const w::EntityHandle p1 = mark(1);
	const w::EntityHandle p2 = mark(2);
	auto marked = [&](w::EntityHandle h) {
		const w::Entity *e = world.registry.get(h);
		return e != nullptr && ((e->flags | e->engine_flags) & w::kEntityFlagPriorityTarget) != 0;
	};
	auto remark = [&]() {
		for (const w::EntityHandle h : {p0, p1, p2}) {
			w::Entity *e = world.registry.get(h);
			e->flags |= w::kEntityFlagPriorityTarget;
			e->engine_flags |= w::kEntityFlagPriorityTarget;
		}
	};
	inmatch::NapiNPServerCtx ctx;
	ctx.world = &world;
	ctx.is_authority = 1;
	inmatch::Server_TickUpdate(ctx);
	if (!expect(!marked(p0) && !marked(p1) && marked(p2) &&
			world.priority_target_clear_countdown == 744,
			"the first authority tick sweeps pools 0/1 only and reloads 744"))
		return false;
	remark();
	for (int i = 0; i < 743; ++i) inmatch::Server_TickUpdate(ctx);
	if (!expect(marked(p0) && marked(p1) && world.priority_target_clear_countdown == 1,
			"743 further ticks leave the marks in place"))
		return false;
	inmatch::Server_TickUpdate(ctx);
	return expect(!marked(p0) && !marked(p1) && marked(p2) &&
					world.priority_target_clear_countdown == 744,
			"the 744th tick sweeps again");
}

// --------------------------------------------------------------------------
// The 1 Hz / accept-time kit-weight recompute.
// --------------------------------------------------------------------------

bool check_kit_weight_recompute() {
	w::World world;
	world.rules.mp_session = true;
	world.registry.configure_pool(0, 8);
	install_rifle_armory(world);
	w::LocalPlayer local(world);
	world.local_player_state = &local;
	const w::EntityHandle host = w::spawn_player(world, player_spawn(0, 0, 0));
	const w::EntityHandle remote = w::spawn_remote_player(world, player_spawn(3, 0, 0));
	if (!expect(host.valid() && remote.valid() && world.cached.local_player == host,
			"kit-weight fixture spawned host and remote players"))
		return false;

	inmatch::NapiNPServerCtx ctx;
	ctx.world = &world;
	ctx.is_authority = 1;
	ctx.is_in_session = 1;
	ctx.config.game_type = game_type::kTeamDeathmatch;
	w::MatchRules rules;
	rules.game_type = ctx.config.game_type;
	world.match.configure(rules);
	ns::LoopbackChannel host_wire;
	ns::UdpSessionTransport remote_wire(ns::UdpSessionTransport::Role::Host);
	auto &roster = ctx.np_protocol.connection_list;
	roster.push_back(make_seeded_conn(1, 2, &host_wire, ns::TransportMode::Loopback, host, true));
	roster.push_back(make_seeded_conn(3, 1, &remote_wire, ns::TransportMode::Client, remote, true));
	inmatch::NapiNPConnection &remote_conn = roster[1];
	w::Entity *remote_entity = world.registry.get(remote);
	remote_entity->flags |= w::kEntityFlagDead; // the armory-window gate admits a dead player
	const uint16_t combo = 3 * 65 + 2;

	// (2) The accept stamps the remote body: pool 90 - one 30-round clip drawn
	// -> 3 clips total -> 3 u + 3 x 1 u.
	LoadoutSubmit request;
	request.team = 1;
	request.player_class = 8;
	request.weapon_slot_index = combo;
	request.entries.push_back(LoadoutSubmitEntry{5, 3, 0xFF, 0xFF});
	(void)inmatch::dispatch_session_replies(ctx.config, remote_conn,
			{make_protocol_message(c2s::LOADOUT_SUBMIT, encode_loadout_submit(request))},
			100, roster, &world);
	auto remote_weight = [&]() -> int32_t {
		const w::AiEntity *body = world.ai.for_handle(remote);
		return body != nullptr ? body->inf.loadout_weight_fp16 : -1;
	};
	if (!expect(remote_conn.reply.ammo_pools[0] == 60 &&
			slots_of(world, remote_conn)[combo].clip == 30 && remote_weight() == 0x60000,
			"the loadout accept seeds the host-side rows and stamps the remote kit weight"))
		return false;
	remote_entity->flags &= ~w::kEntityFlagDead;

	// Spend the clip through C2S 0x06: the weight lightens on the next periodic
	// second, never between.
	uint32_t client_tick = remote_conn.tick_seed;
	auto fire = [&](int shots) {
		for (int i = 0; i < shots; ++i) {
			(void)inmatch::dispatch_session_replies(ctx.config, remote_conn,
					{make_protocol_message(c2s::FIRED_ROUND,
							fire_body(remote_conn, remote.packed, 5, client_tick))},
					100, roster, &world);
		}
	};
	tick_to_periodic_second(ctx); // the periodic second that precedes the shots
	fire(30);
	if (!expect(slots_of(world, remote_conn)[combo].clip == 0 &&
			remote_weight() == 0x60000,
			"a spent clip does not change the weight before the periodic second"))
		return false;
	tick_to_periodic_second(ctx);
	if (!expect(remote_weight() == 0x50000,
			"the periodic second lightens the remote kit by the spent clip"))
		return false;
	// The reload relay moves rounds pool -> clip, so the total is conserved.
	{
		WeaponReload reload;
		reload.entity_handle = remote.packed;
		reload.reload_param = combo;
		(void)inmatch::dispatch_session_replies(ctx.config, remote_conn,
				{make_protocol_message(c2s::WEAPON_RELOAD_REQUEST, encode_weapon_reload(reload))},
				100, roster, &world);
	}
	if (!expect(slots_of(world, remote_conn)[combo].clip == 30 && remote_conn.reply.ammo_pools[0] == 30,
			"the host-side reload draws the clip out of the ammo pool"))
		return false;
	tick_to_periodic_second(ctx);
	if (!expect(remote_weight() == 0x50000,
			"a reload leaves the total kit weight unchanged"))
		return false;
	fire(30);
	tick_to_periodic_second(ctx);
	if (!expect(remote_weight() == 0x40000,
			"the next spent clip lightens the kit again"))
		return false;

	// (1) The listen host's own player: its live inventory is the row source.
	local.inventory.reset(world.tables.weapons);
	w::WeaponInventorySlot *slot = local.inventory.slot(combo);
	slot->adm_index = 5;
	slot->clip = 30;
	local.inventory.pools[0] = 60;
	local.inventory_valid = true;
	tick_to_periodic_second(ctx);
	if (!expect(local.loadout.weight_fp16 == 0x60000,
			"the periodic second stamps the host's own kit weight from its live inventory"))
		return false;
	slot->clip = 0;
	inmatch::Server_TickUpdate(ctx);
	if (!expect(local.loadout.weight_fp16 == 0x60000,
			"the host's own weight only moves on the periodic second"))
		return false;
	tick_to_periodic_second(ctx);
	drain(host_wire);
	drain(remote_wire);
	return expect(local.loadout.weight_fp16 == 0x50000,
			"the host's own spent clip lightens its kit on the periodic second");
}

// The kit weight reads its clip counts through the player's seat: on a UseGun seat
// the rifle's count is the carrier's MountSlot reserve plus the slot's own clip.
// [orig: Server_RecalculateAllPlayerScores @0x5014E0 - WeaponSlot_GetTotalClips
//  @0x501546 with the slot's entity; Entity_GetScoreValueBySlotType @0x540707..0x540774]
bool check_kit_weight_reads_the_seat() {
	w::World world;
	world.rules.mp_session = true;
	world.registry.configure_pool(0, 8);
	world.registry.configure_pool(1, 4);
	install_rifle_armory(world);
	const w::EntityHandle remote = w::spawn_remote_player(world, player_spawn(3, 0, 0));
	std::vector<inmatch::NapiNPConnection> roster;
	roster.push_back(make_seeded_conn(3, 1, nullptr, ns::TransportMode::Client, remote, true));
	inmatch::NapiNPConnection &conn = roster.front();
	slots_of(world, conn)[3 * 65 + 2] = w::PersonalWeaponSlot{5, 30};
	conn.reply.ammo_pools[0] = 60;
	auto weight = [&]() -> int32_t {
		inmatch::Server_RecalculateAllPlayerKitWeights(roster, world);
		const w::AiEntity *body = world.ai.for_handle(remote);
		return body != nullptr ? body->inf.loadout_weight_fp16 : -1;
	};
	if (!expect(weight() == 0x60000, "an unseated rifle counts 60 + 30 -> 3 clips"))
		return false;
	w::Entity carrier;
	carrier.has_item_def = true;
	carrier.primary_weapon_slot.reserve = 90;
	w::Entity *body = world.registry.get(remote);
	body->mounted = true;
	body->mount_type = w::SeatType::Gunner;
	body->mount_target = world.registry.spawn(1, carrier);
	return expect(weight() == 0x70000,
			"a UseGun seat counts the carrier's reserve: 90 + 30 -> 4 clips");
}

// --------------------------------------------------------------------------
// The deploy's kit rebuild from the loadout buffer (D-NET-378).
// --------------------------------------------------------------------------

// Every deploy that is not a medic revive rebuilds the slot's weapon table, ammo
// pools and clips from the last accepted 0x2F, then re-sends the 0x5A built from
// the rebuilt table; a revive keeps the kit the player died with.
// [orig: Server_DeployPlayer @0x5178C5 (the revive test), @0x5178E1
//  (PlayerSlot_InitWeaponsFromLoadout @0x515550), @0x5178F2
//  (Server_SendWeaponSlotListToPlayer); the buffer NapiNPServerMsg_HandlePlayerLoadout
//  @0x515CBD..0x515D83]
bool check_deploy_rebuilds_the_kit_from_the_loadout_buffer() {
	w::World world;
	world.rules.mp_session = true;
	world.registry.configure_pool(0, 8);
	install_rifle_armory(world);
	const w::EntityHandle remote = w::spawn_remote_player(world, player_spawn(3, 0, 0));
	if (!expect(remote.valid(), "the deploy fixture spawned a remote player")) return false;
	inmatch::NapiNPServerCtx ctx;
	ctx.world = &world;
	ctx.is_authority = 1;
	ctx.is_in_session = 1;
	ctx.config.game_type = game_type::kTeamDeathmatch;
	w::MatchRules rules;
	rules.game_type = ctx.config.game_type;
	world.match.configure(rules);
	ns::UdpSessionTransport remote_wire(ns::UdpSessionTransport::Role::Host);
	auto &roster = ctx.np_protocol.connection_list;
	roster.push_back(make_seeded_conn(3, 1, &remote_wire, ns::TransportMode::Client, remote, true));
	inmatch::NapiNPConnection &conn = roster[0];
	w::Entity *body = world.registry.get(remote);
	const uint16_t combo = 3 * 65 + 2;

	body->flags |= w::kEntityFlagDead; // the armory-window gate admits a dead player
	LoadoutSubmit request;
	request.team = 1;
	request.player_class = 8;
	request.weapon_slot_index = combo;
	request.entries.push_back(LoadoutSubmitEntry{5, 3, 0xFF, 0xFF});
	(void)inmatch::dispatch_session_replies(ctx.config, conn,
			{make_protocol_message(c2s::LOADOUT_SUBMIT, encode_loadout_submit(request))},
			100, roster, &world);
	const std::vector<uint8_t> granted = conn.reply.last_loadout_reply;
	if (!expect(conn.reply.loadout_buffer_set && conn.reply.ammo_pools[0] == 60 &&
			slots_of(world, conn)[combo].clip == 30 && !granted.empty(),
			"the accept keeps the buffer and seeds 60 + one 30-round clip"))
		return false;
	body->flags &= ~w::kEntityFlagDead;

	uint32_t client_tick = conn.tick_seed;
	auto spend_clip = [&]() {
		for (int i = 0; i < 30; ++i)
			(void)inmatch::dispatch_session_replies(ctx.config, conn,
					{make_protocol_message(c2s::FIRED_ROUND,
							fire_body(conn, remote.packed, 5, client_tick))},
					100, roster, &world);
	};
	spend_clip();
	conn.reply.ammo_pools[0] = 7; // a reload or two in the last life
	if (!expect(slots_of(world, conn)[combo].clip == 0, "the last life spent the clip")) return false;

	auto deploy = [&]() {
		body->flags |= w::kEntityFlagDead;
		body->alive = false;
		body->health = 0;
		return inmatch::Server_ReleasePlayerDeployment(ctx.config, conn, world, {}, 0xFFFF);
	};
	auto loadout_body = [](const std::vector<ProtocolMessage> &replies) {
		for (const ProtocolMessage &m : replies)
			if (m.tag == 0x5A) return m.payload;
		return std::vector<uint8_t>{};
	};
	std::vector<ProtocolMessage> replies = deploy();
	if (!expect(conn.reply.ammo_pools[0] == 60 && slots_of(world, conn)[combo].clip == 30,
			"the deploy rebuilds the pool and the clip from the loadout buffer"))
		return false;
	if (!expect(loadout_body(replies) == granted,
			"the deploy's 0x5A is the rebuilt table, the accept's body again"))
		return false;

	// A medic revive's deploy keeps the spent kit (no rebuild) and sends no 0x5A,
	// only the seed reroll (D-NET-379) [orig: Server_DeployPlayer @0x5178C5
	// skips @0x5178E1 and @0x5178F2; the 0x61 @0x51796D].
	client_tick = conn.tick_seed;
	spend_clip();
	conn.reply.ammo_pools[0] = 7;
	conn.reply.revive_pose_valid = true;
	replies = deploy();
	if (!expect(conn.reply.ammo_pools[0] == 7 && slots_of(world, conn)[combo].clip == 0,
			"a revive deploy leaves the kit the player died with"))
		return false;
	bool seed = false;
	for (const ProtocolMessage &m : replies) seed = seed || m.tag == 0x61;
	return expect(loadout_body(replies).empty() && seed,
			"a revive deploy sends the 0x61 reroll and no 0x5A");
}

// A slot that never submitted deploys into an empty table: the rebuild runs over
// its empty buffer and list, so its rows go and every pool is zeroed.
// [orig: PlayerSlot_InitWeaponsFromLoadout @0x515550 - the buffer test @0x5155EB skips
//  the entry walk alone; WeaponSlots_SeedAmmoPoolsFromDefs @0x54169C..0x5416A5]
bool check_deploy_rebuilds_an_unsubmitted_slot_empty() {
	w::World world;
	world.rules.mp_session = true;
	world.registry.configure_pool(0, 8);
	install_rifle_armory(world);
	const w::EntityHandle remote = w::spawn_remote_player(world, player_spawn(3, 0, 0));
	inmatch::NapiNPServerCtx ctx;
	ctx.world = &world;
	ctx.is_authority = 1;
	ctx.is_in_session = 1;
	ctx.config.game_type = game_type::kTeamDeathmatch;
	w::MatchRules rules;
	rules.game_type = ctx.config.game_type;
	world.match.configure(rules);
	ns::UdpSessionTransport remote_wire(ns::UdpSessionTransport::Role::Host);
	auto &roster = ctx.np_protocol.connection_list;
	roster.push_back(make_seeded_conn(3, 1, &remote_wire, ns::TransportMode::Client, remote, true));
	inmatch::NapiNPConnection &conn = roster[0];
	slots_of(world, conn)[3 * 65 + 2] = w::PersonalWeaponSlot{5, 30}; // a picked-up rifle
	conn.reply.ammo_pools[0] = 40;
	w::Entity *body = world.registry.get(remote);
	body->flags |= w::kEntityFlagDead;
	body->alive = false;
	body->health = 0;
	(void)inmatch::Server_ReleasePlayerDeployment(ctx.config, conn, world, {}, 0xFFFF);
	return expect(slots_of(world, conn).empty() && conn.reply.ammo_pools[0] == 0,
			"an unsubmitted slot's deploy leaves no row and no pool");
}

// The listen host's own slot is kept apart from its local inventory (D-NET-479)
// and takes no 0x2F: its deploy leaves the kit, its carry bits included, and the
// slot's rows, where an empty rebuild would strip them.
// [orig: Server_DeployPlayer @0x5178E1 over the host's slot, whose buffer is
//  g_SpawnLoadoutBuffer (Server_InitAllPlayerEntitiesForRound @0x516BCA..0x516BE3)]
bool check_deploy_leaves_the_listen_hosts_kit() {
	w::World world;
	world.rules.mp_session = true;
	world.registry.configure_pool(0, 8);
	install_rifle_armory(world);
	w::LocalPlayer local(world);
	world.local_player_state = &local;
	const w::EntityHandle host = w::spawn_player(world, player_spawn(0, 0, 0));
	if (!expect(host.valid() && world.cached.local_player == host,
			"the deploy fixture spawned the listen host"))
		return false;
	inmatch::NapiNPServerCtx ctx;
	ctx.world = &world;
	ctx.is_authority = 1;
	ctx.is_in_session = 1;
	ctx.config.game_type = game_type::kTeamDeathmatch;
	w::MatchRules rules;
	rules.game_type = ctx.config.game_type;
	world.match.configure(rules);
	auto &roster = ctx.np_protocol.connection_list;
	roster.push_back(make_seeded_conn(0, 1, nullptr, ns::TransportMode::Client, host, true));
	inmatch::NapiNPConnection &conn = roster[0];
	slots_of(world, conn)[3 * 65 + 2] = w::PersonalWeaponSlot{5, 30};
	w::Entity *body = world.registry.get(host);
	body->carry_flags |= 0x18u; // the local kit's armor and parachute
	body->flags |= w::kEntityFlagDead;
	body->alive = false;
	body->health = 0;
	(void)inmatch::Server_ReleasePlayerDeployment(ctx.config, conn, world, {}, 0xFFFF);
	body = world.registry.get(host);
	return expect(body != nullptr && (body->carry_flags & 0x18u) == 0x18u &&
					slots_of(world, conn).size() == 1,
			"the listen host's deploy keeps its kit's carry bits and rows");
}

// The deploy's pool leg looks the buffered name up again: of two weapons sharing a name,
// the first takes the entry's count, as the fill does, not the one the index named.
// [orig: PlayerSlot_InitWeaponsFromLoadout @0x515608 / @0x5156AF]
bool check_deploy_pools_follow_the_buffered_name() {
	w::World world;
	world.rules.mp_session = true;
	world.registry.configure_pool(0, 8);
	install_rifle_armory(world);
	w::WeaponTable &table = world.tables.weapons;
	table.ammo_class_names = {"CLS_A", "CLS_B"};
	table.ammo_class_caps = {1000, 1000};
	table.entries.resize(7);
	table.entries[6] = table.entries[5]; // the same name, another ammo class
	table.entries[6].ammo_class = "CLS_B";
	table.entries[6].ammo_class_id = 1;
	const w::EntityHandle remote = w::spawn_remote_player(world, player_spawn(0, 0, 0));
	std::vector<inmatch::NapiNPConnection> roster;
	roster.push_back(make_seeded_conn(3, 1, nullptr, ns::TransportMode::Client, remote, true));
	inmatch::NapiNPConnection &conn = roster.front();
	conn.slot_class = 8;
	conn.reply.loadout_buffer.team = 1;
	conn.reply.loadout_buffer.player_class = 8;
	conn.reply.loadout_buffer.entries.push_back(LoadoutSubmitEntry{6, 2, 0xFF, 0xFF});
	conn.reply.loadout_buffer_set = true;
	inmatch::PlayerSlot_InitWeaponsFromLoadout(conn, world, *world.registry.get(remote));
	return expect(conn.reply.ammo_pools[0] != 0 && conn.reply.ammo_pools[1] == 0,
			"the deploy's pools follow the buffered name's first weapon");
}

// The map change keeps a slot's kit, and the round init rebuilds the slot's table from
// it: the kit's rows, the pools seeded from the defs and one clip drawn, before any
// count (the rifle's 90 start rounds, not the one granted magazine).
// [orig: Server_DisconnectAndResetAllPlayerSlots @0x516160 (no store to +94408 /
//  +88664 / +89176); Server_InitAllPlayerEntitiesForRound @0x516BF3..0x516C20]
bool check_round_init_rebuilds_the_kept_kit() {
	w::World world;
	world.rules.mp_session = true;
	world.registry.configure_pool(0, 8);
	install_rifle_armory(world);
	const w::EntityHandle remote = w::spawn_remote_player(world, player_spawn(3, 0, 0));
	std::vector<inmatch::NapiNPConnection> roster;
	roster.push_back(make_seeded_conn(3, 1, nullptr, ns::TransportMode::Client, remote, true));
	inmatch::NapiNPConnection &conn = roster.front();
	LoadoutSubmit request;
	request.team = 1;
	request.player_class = 8;
	request.weapon_slot_index = 3 * 65 + 2;
	request.entries.push_back(LoadoutSubmitEntry{5, 1, 0xFF, 0xFF});
	conn.link.preround_loadout_latch = true;
	(void)inmatch::dispatch_session_replies(inmatch::GameConfig{}, conn,
			{make_protocol_message(c2s::LOADOUT_SUBMIT, encode_loadout_submit(request))},
			100, roster, &world);
	if (!expect(conn.reply.ammo_pools[0] == 0 && slots_of(world, conn)[3 * 65 + 2].clip == 30,
			"the one-magazine accept draws its clip and leaves no reserve"))
		return false;
	inmatch::reset_slot_for_next_mission(conn);
	if (!expect(conn.reply.loadout_buffer_set &&
					conn.reply.loadout_buffer.entries.size() == 1,
			"the map change keeps the slot's buffer"))
		return false;
	// The rows are the player entity's; the round init rebuilds them on the slot's
	// entity from the kept buffer whatever they held.
	auto &rows = world.registry.get(remote)->weapon_slots;
	rows[3 * 65 + 2].clip = 4; // a spent clip the round init redraws
	rows[3 * 65 + 7] = w::PersonalWeaponSlot{5, 9}; // a row the buffer does not name
	inmatch::PlayerSlot_RebuildKitForRound(conn, world, *world.registry.get(remote));
	return expect(rows.size() == 1 && rows[3 * 65 + 2].clip == 30 &&
					conn.reply.ammo_pools[0] == 60,
			"the round init rebuilds the rifle with its seeded 90 less one clip");
}

// --------------------------------------------------------------------------
// The slot's damage-class bytes behind the S2C 0x5A fourth byte (D-NET-508).
// --------------------------------------------------------------------------

// Each accepted 0x2F entry stores its RAW fourth byte at its weapon's round ammo
// index in the slot's table, which nothing but the add clears; every 0x5A row reads
// the table at the row weapon's OWN AdmDef index, and the impact damage's copy (the
// entity's) follows the slot table onto a kept slot's next entity.
// [orig: NapiNPServerMsg_HandlePlayerLoadout @0x515E8B..0x515E92 (the store, no clear
//  among the accept's memsets @0x515BC1 / @0x515BD3); Server_SendWeaponSlotListToPlayer
//  @0x50286A..0x50288A (the read at AdmDef_FindIndexByName @0x502720); Server_PlayerAdd
//  @0x51CD06; Weapon_CalcImpactDamage @0x4ECA11..0x4ECA24]
bool check_loadout_damage_class_bytes() {
	inmatch::NapiNPServerCtx ctx;
	inmatch::set_connection_mode(ctx, inmatch::ConnectionMode::HostOnly);
	ctx.is_in_session = 1;
	ctx.config.max_players = 4;
	ctx.config.armory_reuse_time = 0; // back-to-back accepts
	w::World world;
	world.rules.mp_session = true;
	world.registry.configure_pool(0, 8);
	world.registry.configure_pool(3, 8);
	install_rifle_armory(world); // adm 5, round ammo index 1
	// adm 1: a pistol whose round is ammo index 5, the rifle's own AdmDef index.
	w::WeaponTableEntry &pistol = world.tables.weapons.entries[1];
	pistol.name = "WPN_TESTPISTOL";
	pistol.valid = true;
	pistol.category = 2;
	pistol.rank = 0;
	pistol.clipsize = 7;
	pistol.startrounds = 21;
	pistol.maxclips = 3;
	pistol.charfilter = 0xFF;
	pistol.teamfilter = 0x03;
	pistol.loadout_selectable = 1;
	pistol.ammo_class = "CLS_A";
	pistol.ammo_class_id = 0;
	pistol.ammo_class_count = 1;
	pistol.ammo_index = 5;
	// adm 2: a medic-only weapon (filtered for class 8) firing ammo 7; adm 3: one whose
	// round index, 128, lies past the 128-byte table.
	for (const uint8_t adm : {uint8_t{2}, uint8_t{3}}) {
		w::WeaponTableEntry &e = world.tables.weapons.entries[adm];
		e = pistol;
		e.name = adm == 2 ? "WPN_TESTMEDIC" : "WPN_TESTWIDE";
		e.rank = adm;
	}
	world.tables.weapons.entries[2].charfilter = 0x01; // medic only
	world.tables.weapons.entries[2].ammo_index = 7;
	world.tables.weapons.entries[3].ammo_index = 128;
	ctx.world = &world;
	ns::UdpSessionTransport transport(ns::UdpSessionTransport::Role::Host);
	inmatch::NapiNPConnection joiner;
	joiner.peer = {0x0100007Fu, 35002};
	joiner.type = 1;
	joiner.connection_id = inmatch::kFirstJoinerDcb;
	joiner.link.mode = ns::TransportMode::Client;
	joiner.link.transport = &transport;
	ctx.np_protocol.connection_list.push_back(std::move(joiner));
	auto &roster = ctx.np_protocol.connection_list;
	inmatch::NapiNPConnection &conn = roster.front();
	if (!expect(inmatch::Server_BuildPlayerInfoAndAdd(ctx, conn, world,
	                    /*round_init=*/false).valid(),
			"the damage-class fixture adds its player"))
		return false;
	auto submit = [&](std::vector<LoadoutSubmitEntry> entries, WeaponLoadout &out) {
		if (w::Entity *body = world.registry.get(conn.link.owned_entity))
			body->flags |= w::kEntityFlagDead; // the armory-window gate admits a dead player
		LoadoutSubmit request;
		request.team = 1;
		request.player_class = 8;
		request.weapon_slot_index = 3 * 65 + 2;
		request.entries = std::move(entries);
		const std::vector<ProtocolMessage> replies = inmatch::dispatch_session_replies(
				ctx.config, conn,
				{make_protocol_message(c2s::LOADOUT_SUBMIT, encode_loadout_submit(request))},
				100, roster, &world);
		for (const ProtocolMessage &m : replies)
			if (m.tag == 0x5A)
				return decode_weapon_loadout(m.payload.data(), m.payload.size(), out);
		return false;
	};
	auto row = [](const WeaponLoadout &lo, uint8_t adm) -> const WeaponLoadoutSlot * {
		for (const WeaponLoadoutSlot &slot : lo.slots)
			if (slot.type_id == adm) return &slot;
		return nullptr;
	};
	auto copy = [&](size_t index) -> int {
		const w::Entity *body = world.registry.get(conn.link.owned_entity);
		return body != nullptr && index < body->ammo_damage_class.size()
				? body->ammo_damage_class[index]
				: -1;
	};
	// The table as the witness leaves it: fixed bytes at fixed indices, zero elsewhere.
	auto table_is = [&](std::initializer_list<std::pair<size_t, uint8_t>> set) {
		std::array<uint8_t, inmatch::NapiNPConnection::kAmmoDamageClassBytes> want{};
		for (const auto &entry : set) want[entry.first] = entry.second;
		const w::Entity *body = world.registry.get(conn.link.owned_entity);
		return conn.ammo_damage_classes == want && body != nullptr &&
		       body->ammo_damage_class == std::vector<uint8_t>(want.begin(), want.end());
	};

	// The rifle's raw 0xFF lands at ammo 1 and the pistol's 2 at ammo 5; each row reads
	// its own AdmDef index, so the pistol (adm 1) shows the rifle's byte and the rifle
	// (adm 5) the pistol's.
	// The medic-only entry is filtered and stores nothing at its ammo 7; the wide one is
	// accepted, but its index 128 lies past the table, so nothing in it changes.
	WeaponLoadout first;
	if (!expect(submit({{5, 3, 0xFF, 0xFF}, {1, 2, 0xFF, 2}, {2, 1, 0xFF, 0x33},
	                    {3, 1, 0xFF, 0x44}},
	                    first),
	            "the first 0x2F is accepted"))
		return false;
	const WeaponLoadoutSlot *pistol_row = row(first, 1);
	const WeaponLoadoutSlot *rifle_row = row(first, 5);
	if (!expect(table_is({{1, 0xFF}, {5, 2}}),
			"the accept stores each raw fourth byte at its weapon's round ammo index, nothing else"))
		return false;
	if (!expect(row(first, 2) == nullptr && row(first, 3) != nullptr && row(first, 3)->ammo_alt == 0,
			"the filtered entry has no row, the wide one reads its own index 3"))
		return false;
	if (!expect(pistol_row != nullptr && pistol_row->ammo_alt == 0xFF && rifle_row != nullptr &&
					rifle_row->ammo_alt == 2,
			"each 0x5A row's fourth byte is the slot table at the row's own AdmDef index"))
		return false;
	if (!expect(copy(1) == 0xFF && copy(5) == 2,
			"the impact damage's copy holds the slot table, raw"))
		return false;

	// A second accept with the rifle alone rewrites ammo 1 and clears nothing: ammo 5
	// keeps the pistol's 2, which the rifle's row still reads.
	WeaponLoadout second;
	if (!expect(submit({{5, 3, 0xFF, 1}}, second), "the second 0x2F is accepted")) return false;
	rifle_row = row(second, 5);
	if (!expect(table_is({{1, 1}, {5, 2}}) && second.slots.size() == 1 && rifle_row != nullptr &&
					rifle_row->ammo_alt == 2,
			"a later accept overwrites its own indices and keeps the rest"))
		return false;
	if (!expect(copy(1) == 1 && copy(5) == 2, "the copy keeps the bytes the accept did not write"))
		return false;

	// The map change's round init gives the kept slot a new entity; its copy is the
	// slot's table, not a fresh zero.
	conn.link.owned_entity = w::EntityHandle{};
	if (!expect(inmatch::Server_BuildPlayerInfoAndAdd(ctx, conn, world,
	                    /*round_init=*/true).valid(),
			"the kept slot's round-init entity spawns"))
		return false;
	return expect(copy(1) == 1 && copy(5) == 2,
			"a kept slot's new entity takes the slot's damage-class bytes");
}

// --------------------------------------------------------------------------
// The 0x2F grant's sub-variant rows (D-NET-509).
// --------------------------------------------------------------------------

// The armory of the sub-variant case: adm 1 an automatic rifle at combo 195 whose
// two loadout_subclasses sub-variants follow it, adm 2 its semi mode at combo 196
// sharing its ammo class and loaded-round bucket, and adm 3 its launcher at combo 200,
// outside the rifle's two skipped slots, on a class of its own with a two-round clip;
// adm 4 another weapon on the rifle's combo 195. Their ammo classes are 0, 2 and 3:
// ammo class 1 reads its own legs (a controller seat reads it as 0).
void install_sub_variant_armory(w::World &world) {
	w::WeaponTable &table = world.tables.weapons;
	table.entries.resize(5);
	auto weapon = [&](uint8_t adm, const char *name, int32_t rank, int32_t clipsize,
			int32_t startrounds, int32_t maxclips, int32_t ammo_class) -> w::WeaponTableEntry & {
		w::WeaponTableEntry &e = table.entries[adm];
		e.name = name;
		e.valid = true;
		e.category = 3;
		e.rank = rank;
		e.clipsize = clipsize;
		e.startrounds = startrounds;
		e.maxclips = maxclips;
		e.charfilter = 0xFF;
		e.teamfilter = 0x03;
		e.loadout_selectable = 1;
		e.ammo_class_id = ammo_class;
		e.ammo_class = table.ammo_class_names[static_cast<size_t>(ammo_class)];
		e.ammo_class_count = 1;
		e.ammo_index = adm;
		return e;
	};
	table.ammo_class_names = {"CLS_A", "CLS_ONE", "CLS_B", "CLS_C"};
	table.ammo_class_caps = {1000, 1000, 1000, 1000};
	w::WeaponTableEntry &automatic = weapon(1, "WPN_TESTAUTO", 0, 30, 300, 10, 0);
	automatic.loadout_subclasses = 2;
	automatic.ammo_bucket = 1;
	weapon(2, "WPN_TESTSEMI", 1, 30, 300, 10, 0).ammo_bucket = 1;
	weapon(3, "WPN_TESTGL", 5, 2, 6, 6, 2);
	weapon(4, "WPN_TESTOTHER", 0, 10, 50, 5, 3);
}

// The accept loads each kit weapon's sub-variants into the slot's table beside it,
// the first weapon on a combo keeping it; the 0x5A walk skips each row's sub-variant
// slots, so a sub-variant outside them is a row of its own, and a row's second count
// is the first other-class sub-variant's. A negative second count takes that
// sub-variant's start rounds times its clipsize at the accept and unmultiplied at the
// deploy's rebuild.
// [orig: NapiNPServerMsg_HandlePlayerLoadout - AvatarDef_BuildDisplayList @0x515DA6,
//  WeaponSlotTable_LoadAllFromDefs @0x515DBF (the incumbent kept @0x5415D6), the
//  sub-variant pools @0x515E99..0x515F33; PlayerSlot_InitWeaponsFromLoadout
//  @0x5156EF..0x515752; Server_SendWeaponSlotListToPlayer @0x5027B3..0x50284E]
bool check_loadout_sub_variant_rows() {
	w::World world;
	world.rules.mp_session = true;
	world.registry.configure_pool(0, 8);
	world.registry.configure_pool(1, 4);
	install_sub_variant_armory(world);
	const w::EntityHandle remote = w::spawn_remote_player(world, player_spawn(3, 0, 0));
	if (!expect(remote.valid(), "the sub-variant fixture spawned a remote player")) return false;
	inmatch::NapiNPServerCtx ctx;
	ctx.world = &world;
	ctx.is_authority = 1;
	ctx.is_in_session = 1;
	ctx.config.game_type = game_type::kTeamDeathmatch;
	w::MatchRules rules;
	rules.game_type = ctx.config.game_type;
	world.match.configure(rules);
	ns::UdpSessionTransport remote_wire(ns::UdpSessionTransport::Role::Host);
	auto &roster = ctx.np_protocol.connection_list;
	roster.push_back(make_seeded_conn(3, 1, &remote_wire, ns::TransportMode::Client, remote, true));
	inmatch::NapiNPConnection &conn = roster[0];
	w::Entity *body = world.registry.get(remote);

	body->flags |= w::kEntityFlagDead; // the armory-window gate admits a dead player
	LoadoutSubmit request;
	request.team = 1;
	request.player_class = 8;
	request.weapon_slot_index = 3 * 65;
	request.entries.push_back(LoadoutSubmitEntry{1, 0xFF, 0xFF, 0});
	request.entries.push_back(LoadoutSubmitEntry{4, 0xFF, 0xFF, 0});
	(void)inmatch::dispatch_session_replies(ctx.config, conn,
			{make_protocol_message(c2s::LOADOUT_SUBMIT, encode_loadout_submit(request))},
			100, roster, &world);
	WeaponLoadout granted;
	if (!expect(decode_weapon_loadout(conn.reply.last_loadout_reply.data(),
					conn.reply.last_loadout_reply.size(), granted),
			"the accept's 0x5A decodes"))
		return false;
	// The rifle's pool 300 draws a clip into the shared bucket twice (the semi mode
	// returns it first): 270 + 30 -> 10 clips. The launcher's 6 x 2 = 12 draws one
	// clip: 10 + 2 -> 6 clips, its own row and the rifle's second count.
	if (!expect(granted.slots.size() == 2 &&
					granted.slots[0].type_id == 1 && granted.slots[0].ammo_primary == 10 &&
					granted.slots[0].ammo_secondary == 6 &&
					granted.slots[1].type_id == 3 && granted.slots[1].ammo_primary == 6 &&
					granted.slots[1].ammo_secondary == 0xFF,
			"the rifle's row skips its semi mode; the launcher outside the skip is a row"))
		return false;
	auto row_adm = [&](uint16_t combo) -> int {
		const auto it = slots_of(world, conn).find(combo);
		return it != slots_of(world, conn).end() ? it->second.adm_index : -1;
	};
	if (!expect(slots_of(world, conn).size() == 3 && row_adm(195) == 1 && row_adm(196) == 2 &&
					row_adm(200) == 3,
			"the host rows hold the rifle, its semi mode and its launcher; the rifle keeps its combo"))
		return false;
	if (!expect(conn.reply.ammo_pools[0] == 270 && conn.reply.ammo_pools[2] == 10 &&
					conn.reply.shared_clips[1] == 30 && slots_of(world, conn)[200].clip == 2,
			"the pools and the shared bucket after the accept's draw"))
		return false;

	// The deploy's rebuild takes the launcher's start rounds unmultiplied: 6 draws one
	// clip, 4 + 2 -> 3 clips.
	body->flags |= w::kEntityFlagDead;
	body->alive = false;
	body->health = 0;
	const std::vector<ProtocolMessage> replies =
			inmatch::Server_ReleasePlayerDeployment(ctx.config, conn, world, {}, 0xFFFF);
	WeaponLoadout deployed;
	bool decoded = false;
	for (const ProtocolMessage &m : replies)
		if (m.tag == 0x5A)
			decoded = decode_weapon_loadout(m.payload.data(), m.payload.size(), deployed);
	if (!expect(decoded && deployed.slots.size() == 2 && deployed.slots[0].type_id == 1 &&
					deployed.slots[0].ammo_primary == 10 &&
					deployed.slots[0].ammo_secondary == 3 && deployed.slots[1].type_id == 3 &&
					deployed.slots[1].ammo_primary == 3 && conn.reply.ammo_pools[2] == 4,
			"the deploy's rebuild sets the launcher's pool from its start rounds alone"))
		return false;

	auto accept = [&](std::vector<LoadoutSubmitEntry> entries) {
		LoadoutSubmit kit = request;
		kit.entries = std::move(entries);
		conn.link.preround_loadout_latch = true; // the post-deploy submit bypasses the window
		conn.link.armory_reuse_seconds = 0;
		(void)inmatch::dispatch_session_replies(ctx.config, conn,
				{make_protocol_message(c2s::LOADOUT_SUBMIT, encode_loadout_submit(kit))},
				100, roster, &world);
	};
	// The seed zeroes every pool first: a kit of the other weapon alone leaves the
	// rifle's and the launcher's classes empty, its own 50 less one 10-round clip.
	// [orig: WeaponSlots_SeedAmmoPoolsFromDefs @0x54169C..0x5416A5]
	accept({{4, 0xFF, 0xFF, 0}});
	if (!expect(conn.reply.ammo_pools[0] == 0 && conn.reply.ammo_pools[2] == 0 &&
					conn.reply.ammo_pools[3] == 40 && conn.reply.shared_clips[1] == 0 &&
					slots_of(world, conn).size() == 1 && slots_of(world, conn)[195].adm_index == 4,
			"a kit change leaves the dropped classes at zero"))
		return false;

	// From a UseGun seat every redraw returns at once and no bucket is written: the
	// pools are the counts, no clip is drawn. [orig: WeaponSlots_RecalculateAmmoFromCapacity
	//  @0x54228D..0x542294; WeaponSlots_ClearLoadedRounds @0x5423C5]
	body->flags &= ~w::kEntityFlagDead;
	body->alive = true;
	body->health = 100;
	body->mounted = true;
	body->mount_type = w::SeatType::Gunner;
	accept({{1, 0xFF, 0xFF, 0}});
	if (!expect(conn.reply.ammo_pools[0] == 300 && conn.reply.ammo_pools[2] == 12 &&
					conn.reply.ammo_pools[3] == 0 && conn.reply.shared_clips[1] == 0 &&
					slots_of(world, conn)[200].clip == 0,
			"a UseGun seat's accept draws no clip"))
		return false;

	// From a controller seat under an EWeap carrier each draw reads the carrier's
	// reserve and spends nothing, and no bucket is written: the launcher's slot takes
	// min(2, 1) rounds, the rifle's bucket none. [orig: Entity_GetScoreValueBySlotType
	//  @0x540727..0x540769; WeaponSlot_DecrementAmmo @0x540920; WeaponSlot_GetSharedLoadedRounds @0x540624;
	//  WeaponSlot_SetSharedLoadedRounds @0x540684..0x540692]
	w::Entity carrier;
	carrier.has_item_def = true;
	carrier.item_attrib = w::kItemAttribEweap;
	carrier.primary_weapon_slot.reserve = 1;
	body->mount_type = w::SeatType::Controller;
	body->mount_target = world.registry.spawn(1, carrier);
	accept({{1, 0xFF, 0xFF, 0}});
	WeaponLoadout seated;
	return expect(decode_weapon_loadout(conn.reply.last_loadout_reply.data(),
					conn.reply.last_loadout_reply.size(), seated) &&
					seated.slots.size() == 2 && seated.slots[0].ammo_primary == 10 &&
					seated.slots[0].ammo_secondary == 6 && conn.reply.ammo_pools[0] == 300 &&
					conn.reply.ammo_pools[2] == 12 && conn.reply.shared_clips[1] == 0 &&
					slots_of(world, conn)[200].clip == 1,
			"a controller seat's draws read the carrier's reserve and spend nothing");
}

// After the grant a stock client's semi-mode and launcher fires find their slots, and
// a weapon on a combo the kit's rifle holds fires the rifle; a combo the kit left
// empty refuses (B-15 over D-NET-509's rows).
// [orig: Server_ClientFiredRound @0x50C0C2..0x50C169, the slot's def firing @0x50C297;
//  the grant's fill @0x515DBF]
bool check_sub_variant_fire_after_the_grant() {
	w::World world;
	world.rules.mp_session = true;
	world.registry.configure_pool(0, 8);
	install_sub_variant_armory(world);
	// The shooter stands at the fire's claimed origin (fire_body claims 0, 0, 0), inside
	// the fire action's two-metre reach (D-NET-751).
	const w::EntityHandle shooter = w::spawn_remote_player(world, player_spawn(0, 0, 0));
	std::vector<inmatch::NapiNPConnection> roster;
	roster.push_back(make_seeded_conn(3, 1, nullptr, ns::TransportMode::Client, shooter, true));
	inmatch::NapiNPConnection &conn = roster.front();
	LoadoutSubmit request;
	request.team = 1;
	request.player_class = 8;
	request.weapon_slot_index = 3 * 65;
	request.entries.push_back(LoadoutSubmitEntry{1, 0xFF, 0xFF, 0});
	conn.link.preround_loadout_latch = true; // the post-join submit bypasses the window
	(void)inmatch::dispatch_session_replies(inmatch::GameConfig{}, conn,
			{make_protocol_message(c2s::LOADOUT_SUBMIT, encode_loadout_submit(request))},
			100, roster, &world);
	uint32_t client_tick = conn.tick_seed;
	auto fire = [&](uint8_t adm) {
		(void)inmatch::dispatch_session_replies(inmatch::GameConfig{}, conn,
				{make_protocol_message(c2s::FIRED_ROUND,
						fire_body(conn, shooter.packed, adm, client_tick))},
				100, roster, &world);
	};
	fire(2); // the semi mode
	fire(3); // the launcher
	if (!expect(world.out.rounds.count == 2 && world.out.rounds.records[0].adm_index == 2 &&
					world.out.rounds.records[1].adm_index == 3 &&
					conn.reply.shared_clips[1] == 29 && slots_of(world, conn)[200].clip == 1,
			"the semi mode spends the shared bucket and the launcher its own clip"))
		return false;
	fire(4); // another weapon on the rifle's combo
	if (!expect(world.out.rounds.count == 3 && world.out.rounds.records[2].adm_index == 1 &&
					conn.reply.shared_clips[1] == 28,
			"a weapon on the rifle's combo fires the rifle"))
		return false;
	world.tables.weapons.entries[4].rank = 7;
	fire(4);
	return expect(world.out.rounds.count == 3 && slots_of(world, conn).count(3 * 65 + 7) == 0,
			"a combo the kit left empty refuses the fire and makes no slot");
}

// --------------------------------------------------------------------------
// SpawnWaveList_TryQueuePlayer's unconditional other-row eviction.
// --------------------------------------------------------------------------

bool check_try_queue_evicts_on_failed_pick() {
	w::World world;
	world.registry.configure_pool(0, 16);
	world.registry.configure_pool(2, 4);
	auto make_player = [&](uint8_t team) {
		w::Entity e;
		e.kind = w::EntityKind::Organic;
		e.team = team;
		return world.registry.spawn(0, e);
	};
	auto make_zone = [&](uint8_t team, uint8_t number) {
		w::Entity z;
		z.kind = w::EntityKind::Item;
		z.team = team;
		z.alive = true;
		z.is_spawn_point = true;
		z.zone_number = number;
		z.zone_control = 0x10000;
		return world.registry.spawn(2, z);
	};
	const w::EntityHandle zone_a = make_zone(1, 1);
	const w::EntityHandle zone_b = make_zone(1, 2);
	const w::EntityHandle zone_c = make_zone(2, 0);
	world.zones.spawn_waves.build_from_mission(world, 5, 5);
	w::SpawnWaveList &waves = world.zones.spawn_waves;
	auto row = [&](w::EntityHandle zone) -> const w::SpawnWaveEntry * {
		for (const w::SpawnWaveEntry &entry : waves.entries())
			if (entry.zone == zone) return &entry;
		return nullptr;
	};
	auto holds = [&](w::EntityHandle zone, w::EntityHandle player) {
		const w::SpawnWaveEntry *entry = row(zone);
		if (entry == nullptr) return false;
		for (const w::EntityHandle member : entry->queued)
			if (member == player) return true;
		return false;
	};
	const w::EntityHandle p0 = make_player(1);
	if (!expect(waves.try_queue(zone_a, p0, 1) && holds(zone_a, p0),
			"the first pick queues at zone A"))
		return false;
	for (int i = 0; i < 8; ++i) {
		if (!expect(waves.try_queue(zone_b, make_player(1), 1), "zone B fills to eight"))
			return false;
	}
	if (!expect(!waves.try_queue(zone_b, p0, 1) && !holds(zone_a, p0) && !holds(zone_b, p0),
			"a full-row pick fails AND evicts the player from its previous zone"))
		return false;
	if (!expect(waves.try_queue(zone_a, p0, 1) && holds(zone_a, p0),
			"the player re-queues at zone A"))
		return false;
	if (!expect(!waves.try_queue(zone_c, p0, 1) && !holds(zone_a, p0) && !holds(zone_c, p0),
			"an other-team zone pick fails AND evicts the player everywhere"))
		return false;
	if (!expect(waves.try_queue(zone_a, p0, 1) && !waves.try_queue(zone_a, p0, 1) &&
			holds(zone_a, p0) && row(zone_a)->queued.size() == 1,
			"a duplicate pick fails but keeps the player in that row"))
		return false;
	return expect(row(zone_b)->queued.size() == 8, "the other rows keep their members");
}

// --------------------------------------------------------------------------
// C2S 0x0E: the Conquer & Control auto-pick rejection and the handle-0 admission.
// --------------------------------------------------------------------------

bool check_respawn_pick_admission() {
	w::World world;
	world.registry.configure_pool(0, 4);
	world.registry.configure_pool(2, 4);
	world.registry.configure_pool(3, 4);
	w::Entity player_seed;
	player_seed.kind = w::EntityKind::Organic;
	player_seed.flags = w::kEntityFlagPlayer | w::kEntityFlagDead;
	player_seed.engine_flags = player_seed.flags;
	player_seed.team = 1;
	player_seed.alive = false;
	player_seed.health = 0;
	player_seed.health_max = 100;
	const w::EntityHandle player = world.registry.spawn(0, player_seed);
	if (!expect(player.pool() == 0 && player.slot() == 0,
			"the player occupies pool 0 index 0 (the handle-0 target)"))
		return false;

	inmatch::GameConfig config;
	config.game_type = game_type::kConquerAndControl;
	std::vector<inmatch::NapiNPConnection> roster(1);
	auto &conn = roster.front();
	conn.type = 1;
	conn.phase = inmatch::ConnectionPhase::InMatch;
	conn.burst.spawned = true;
	conn.link.owned_entity = player;
	conn.link.respawn_pending = true;
	conn.assigned_team = 1; // the slot team (+416) the handler reads
	conn.assigned_team_valid = true;
	conn.reply.player_slot = 0;
	auto reset_player = [&]() {
		w::Entity *entity = world.registry.get(player);
		entity->alive = false;
		entity->health = 0;
		entity->flags |= w::kEntityFlagDead;
		entity->engine_flags |= w::kEntityFlagDead;
		conn.link.respawn_pending = true;
		conn.link.respawn_delay_seconds = 0;
		conn.link.spawn_target_hold_seconds = 0;
	};
	auto dispatch = [&](const std::vector<uint8_t> &body) {
		return inmatch::dispatch_session_replies(config, conn,
				{make_protocol_message(c2s::RESPAWN_REQUEST, body)}, 100, roster, &world);
	};

	reset_player();
	if (!expect(dispatch({0xFE, 0xFF}).empty() && world.registry.get(player)->health == 0,
			"Conquer & Control rejects the 0xFFFE auto pick without a deploy or reply"))
		return false;
	config.game_type = game_type::kAdvanceAndSecure;
	reset_player();
	if (!expect(!dispatch({0xFE, 0xFF}).empty() && world.registry.get(player)->health > 0,
			"the same auto pick deploys under Advance & Secure"))
		return false;
	reset_player();
	if (!expect(dispatch({}).empty() && world.registry.get(player)->health == 0,
			"an absent body reads handle 0 and is rejected (pool 0 index 0 is no spawn point)"))
		return false;
	reset_player();
	if (!expect(dispatch({0x00, 0x00}).empty() && world.registry.get(player)->health == 0,
			"an explicit handle 0 is rejected the same way"))
		return false;
	reset_player();
	if (!expect(!dispatch({0xFF, 0xFF}).empty() && world.registry.get(player)->health > 0,
			"only 0xFFFF is the Default Spawn pick"))
		return false;
	// Pool 0 index 0 empty: the resolver finds nothing and the pick is still rejected.
	w::World empty_world;
	empty_world.registry.configure_pool(0, 4);
	w::Entity later = player_seed;
	empty_world.registry.spawn(0, later);
	const w::EntityHandle second = empty_world.registry.spawn(0, later);
	empty_world.registry.despawn(w::EntityHandle::make(0, 0));
	conn.link.owned_entity = second;
	auto dispatch_empty = [&](const std::vector<uint8_t> &body) {
		return inmatch::dispatch_session_replies(config, conn,
				{make_protocol_message(c2s::RESPAWN_REQUEST, body)}, 100, roster, &empty_world);
	};
	conn.link.respawn_pending = true;
	return expect(dispatch_empty({0x00, 0x00}).empty() &&
					empty_world.registry.get(second)->health == 0,
			"handle 0 over an empty pool-0 row is rejected");
}

// C2S 0x0E under the medic-revive latch (slot+89932): the revived player deploys
// with no target, where its body lay. A picked handle is dropped before the
// resolve, the auto pick's frontier zone becomes -1 (so it never joins that
// zone's wave), and `nodefaultspawnpoints` lets it through.
// [orig: Server_ProcessClientRequestRespawn @0x519AF0 — @0x519c3d..0x519c4b,
//  @0x519c5a, @0x519c97]
bool check_revived_pick_takes_no_target() {
	w::World world;
	world.registry.configure_pool(0, 4);
	world.registry.configure_pool(2, 4);
	w::Entity player_seed;
	player_seed.kind = w::EntityKind::Organic;
	player_seed.flags = w::kEntityFlagPlayer | w::kEntityFlagDead;
	player_seed.engine_flags = player_seed.flags;
	player_seed.team = 1;
	player_seed.alive = false;
	player_seed.health = 0;
	player_seed.health_max = 100;
	const w::EntityHandle player = world.registry.spawn(0, player_seed);
	w::Entity zone_seed;
	zone_seed.kind = w::EntityKind::Item;
	zone_seed.team = 1;
	zone_seed.alive = true;
	zone_seed.is_spawn_point = true;
	zone_seed.zone_control = 0x10000;
	const w::EntityHandle zone = world.registry.spawn(2, zone_seed);
	world.zones.spawn_waves.build_from_mission(world, 5, 5); // the zone's wave row

	inmatch::GameConfig config;
	config.game_type = game_type::kObjectiveCoop; // the auto pick takes the unnumbered zone
	config.default_spawn_requires_no_team_zone = 1;
	std::vector<inmatch::NapiNPConnection> roster(1);
	auto &conn = roster.front();
	conn.type = 1;
	conn.phase = inmatch::ConnectionPhase::InMatch;
	conn.burst.spawned = true;
	conn.link.owned_entity = player;
	conn.assigned_team = 1; // the slot team (+416) the handler reads
	conn.assigned_team_valid = true;
	conn.reply.player_slot = 0;
	auto reset_player = [&](bool revived) {
		w::Entity *entity = world.registry.get(player);
		entity->alive = false;
		entity->health = 0;
		entity->flags |= w::kEntityFlagDead;
		entity->engine_flags |= w::kEntityFlagDead;
		conn.link.respawn_pending = true;
		conn.link.respawn_delay_seconds = 0;
		conn.link.spawn_target_hold_seconds = 0;
		world.zones.spawn_waves.remove_player(player);
		conn.reply.revive_pose_valid = revived;
		conn.reply.revive_pos[0] = 7 << 16;
		conn.reply.revive_pos[1] = 8 << 16;
		conn.reply.revive_pos[2] = 9 << 16;
	};
	auto dispatch = [&](uint16_t pick) {
		return inmatch::dispatch_session_replies(config, conn,
				{make_protocol_message(c2s::RESPAWN_REQUEST,
						{static_cast<uint8_t>(pick), static_cast<uint8_t>(pick >> 8)})},
				100, roster, &world);
	};
	auto has_tag = [](const std::vector<ProtocolMessage> &replies, uint8_t tag) {
		for (const ProtocolMessage &m : replies)
			if (m.tag == tag) return true;
		return false;
	};
	auto queued = [&]() {
		for (const w::SpawnWaveEntry &entry : world.zones.spawn_waves.entries())
			for (const w::EntityHandle member : entry.queued)
				if (member == player) return true;
		return false;
	};

	// Unrevived, the auto pick resolves the zone and queues in its wave.
	reset_player(false);
	std::vector<ProtocolMessage> replies = dispatch(w::kDeployPickAutoTeam);
	if (!expect(has_tag(replies, s2c::SPAWN_WAVE_STATUS) && queued() &&
			world.registry.get(player)->health == 0,
			"an ordinary auto pick queues at the team's zone"))
		return false;
	reset_player(true);
	if (!expect(dispatch(zone.packed).empty() && world.registry.get(player)->health == 0 &&
			conn.reply.revive_pose_valid,
			"a revived player's picked zone is dropped, the latch kept"))
		return false;
	reset_player(true);
	replies = dispatch(w::kDeployPickAutoTeam);
	const w::Entity *body = world.registry.get(player);
	if (!expect(body->health > 0 && !queued() && !has_tag(replies, s2c::SPAWN_WAVE_STATUS) &&
			!conn.reply.revive_pose_valid,
			"a revived player's auto pick deploys at once, outside the zone's wave"))
		return false;
	if (!expect(body->position.x == 7.0f && body->position.y == 8.0f &&
			body->position.z == 9.0f && !has_tag(replies, 0x5A) && has_tag(replies, 0x61),
			"it stands where the revive saved, with the reroll and no 0x5A"))
		return false;
	reset_player(false);
	if (!expect(dispatch(w::kDeployPickNone).empty() && world.registry.get(player)->health == 0,
			"nodefaultspawnpoints refuses Default Spawn while the team holds a zone"))
		return false;
	reset_player(true);
	(void)dispatch(w::kDeployPickNone);
	return expect(world.registry.get(player)->health > 0,
			"the revive latch passes nodefaultspawnpoints");
}

// D-NET-434: a spectator slot's C2S 0x0E. Its no-target and auto picks take a
// substitute team in place of its team 0: 1 under the objective bit (Objective
// Co-op, 0x20000), 2 - (tick & 1) in any other team mode; that team feeds the
// auto pick's zone, `nodefaultspawnpoints` and the numbered zone's team match.
// With a target it skips the wave and deploys at once.
// [orig: Server_ProcessClientRequestRespawn @0x519AF0, the spectator latch
//  @0x519B7A, the slot team @0x519B88, the objective bit's 1 @0x519BA8, the
//  parity @0x519BB4..0x519BC1, Spawn_FindEntityForTeam @0x519BDE,
//  Entity_HasAliveEntityOfTeam @0x519CA8, the zone team @0x519D4A, the
//  spectator's deploy @0x519D65..0x519D7F against SpawnWaveList_TryQueuePlayer
//  @0x519D8F]
bool check_spectator_pick_legs() {
	w::World world;
	world.registry.configure_pool(0, 4);
	world.registry.configure_pool(2, 4);
	w::Entity player_seed;
	player_seed.kind = w::EntityKind::Organic;
	player_seed.flags = w::kEntityFlagPlayer | w::kEntityFlagDead;
	player_seed.engine_flags = player_seed.flags;
	player_seed.team = 0;
	player_seed.alive = false;
	player_seed.health = 0;
	player_seed.health_max = 100;
	const w::EntityHandle player = world.registry.spawn(0, player_seed);
	w::Entity zone_seed;
	zone_seed.kind = w::EntityKind::Item;
	zone_seed.team = 1;
	zone_seed.alive = true;
	zone_seed.is_spawn_point = true;
	zone_seed.zone_control = 0x10000;
	zone_seed.position = {50.0f, 60.0f, 0.0f};
	const w::EntityHandle zone = world.registry.spawn(2, zone_seed);
	world.zones.spawn_waves.build_from_mission(world, 5, 5); // the zone's wave row

	inmatch::GameConfig config;
	config.game_type = game_type::kObjectiveCoop;
	config.default_spawn_requires_no_team_zone = 1;
	std::vector<inmatch::NapiNPConnection> roster(1);
	auto &conn = roster.front();
	conn.type = 1;
	conn.phase = inmatch::ConnectionPhase::InMatch;
	conn.burst.spawned = true;
	conn.link.owned_entity = player;
	conn.link.spectator = true;
	conn.assigned_team = 0; // a spectator slot's +416
	conn.assigned_team_valid = true;
	conn.reply.player_slot = 0;
	auto reset_player = [&]() {
		w::Entity *entity = world.registry.get(player);
		entity->alive = false;
		entity->health = 0;
		entity->team = 0;
		entity->position = {0.0f, 0.0f, 0.0f};
		entity->flags |= w::kEntityFlagDead;
		entity->engine_flags |= w::kEntityFlagDead;
		conn.link.respawn_pending = true;
		conn.link.respawn_delay_seconds = 0;
		conn.link.spawn_target_hold_seconds = 0;
		world.zones.spawn_waves.remove_player(player);
	};
	auto dispatch = [&](uint16_t pick) {
		return inmatch::dispatch_session_replies(config, conn,
				{make_protocol_message(c2s::RESPAWN_REQUEST,
						{static_cast<uint8_t>(pick), static_cast<uint8_t>(pick >> 8)})},
				100, roster, &world);
	};
	auto has_tag = [](const std::vector<ProtocolMessage> &replies, uint8_t tag) {
		for (const ProtocolMessage &m : replies)
			if (m.tag == tag) return true;
		return false;
	};
	auto queued = [&]() {
		for (const w::SpawnWaveEntry &entry : world.zones.spawn_waves.entries())
			for (const w::EntityHandle member : entry.queued)
				if (member == player) return true;
		return false;
	};

	// Objective Co-op: Default Spawn asks as team 1, which holds an available zone.
	reset_player();
	if (!expect(dispatch(w::kDeployPickNone).empty() && world.registry.get(player)->health == 0,
			"an Objective Co-op spectator's Default Spawn is refused by team 1's zone"))
		return false;
	// The auto pick takes team 1's zone and deploys there at once, outside its wave.
	reset_player();
	std::vector<ProtocolMessage> replies = dispatch(w::kDeployPickAutoTeam);
	const w::Entity *body = world.registry.get(player);
	if (!expect(body->health > 0 && !queued() && !has_tag(replies, s2c::SPAWN_WAVE_STATUS) &&
			body->position.x == 50.0f && body->position.y == 60.0f,
			"an Objective Co-op spectator's auto pick deploys at team 1's zone at once"))
		return false;
	// A picked zone (resolved with the spectator's team 0) skips the wave too.
	reset_player();
	replies = dispatch(zone.packed);
	if (!expect(world.registry.get(player)->health > 0 && !queued() &&
			!has_tag(replies, s2c::SPAWN_WAVE_STATUS),
			"a spectator's zone pick deploys at once, outside the zone's wave"))
		return false;
	// A team mode takes the tick parity: odd -> 1, even -> 2.
	config.game_type = game_type::kTeamDeathmatch;
	world.registry.get(zone)->team = 2;
	world.logic_tick = 8;
	reset_player();
	if (!expect(dispatch(w::kDeployPickNone).empty() && world.registry.get(player)->health == 0,
			"on an even tick the spectator asks as team 2, which holds the zone"))
		return false;
	world.logic_tick = 9;
	reset_player();
	(void)dispatch(w::kDeployPickNone);
	return expect(world.registry.get(player)->health > 0,
			"on an odd tick it asks as team 1, which holds none");
}

// D-NET-464: the C2S 0x0E's team base is the slot team (+416), not the entity's,
// and the auto pick's frontier runs through the pick's resolve against the
// requester entity's team like any handle, a miss refusing. A script team write
// to a player body splits the two (Entity_SetTeamByNetId stores the entity byte
// alone). [orig: Server_ProcessClientRequestRespawn @0x519B88, the auto pick's
//  zone @0x519BDE, the frontier's handle @0x519C3D..0x519C54, the resolve
//  @0x519C7C and its miss @0x519C88, Entity_HasAliveEntityOfTeam @0x519CA8, the
//  zone team @0x519D4A; Server_ResolveSpawnTargetHandle @0x4FE175..0x4FE187]
bool check_split_slot_team_pick_legs() {
	w::World world;
	world.registry.configure_pool(0, 4);
	world.registry.configure_pool(2, 4);
	w::Entity player_seed;
	player_seed.kind = w::EntityKind::Organic;
	player_seed.flags = w::kEntityFlagPlayer | w::kEntityFlagDead;
	player_seed.engine_flags = player_seed.flags;
	player_seed.health = 0;
	player_seed.health_max = 100;
	const w::EntityHandle player = world.registry.spawn(0, player_seed);
	auto make_zone = [&](uint8_t team, uint8_t number, float x) {
		w::Entity z;
		z.kind = w::EntityKind::Item;
		z.team = team;
		z.alive = true;
		z.is_spawn_point = true;
		z.zone_number = number;
		z.zone_control = 0x10000;
		z.position = {x, 60.0f, 0.0f};
		return world.registry.spawn(2, z);
	};
	make_zone(1, 0, 50.0f); // the slot team's unnumbered zone (no wave rows)
	const w::EntityHandle other = make_zone(2, 0, 70.0f);
	const w::EntityHandle numbered = make_zone(1, 3, 90.0f);
	inmatch::GameConfig config;
	config.game_type = game_type::kObjectiveCoop; // the auto pick takes the unnumbered zone
	std::vector<inmatch::NapiNPConnection> roster(1);
	auto &conn = roster.front();
	conn.type = 1;
	conn.phase = inmatch::ConnectionPhase::InMatch;
	conn.burst.spawned = true;
	conn.link.owned_entity = player;
	conn.assigned_team = 1;
	conn.assigned_team_valid = true;
	auto reset_player = [&](uint8_t entity_team) {
		w::Entity *entity = world.registry.get(player);
		entity->alive = false;
		entity->health = 0;
		entity->team = entity_team;
		entity->position = {0.0f, 0.0f, 0.0f};
		entity->flags |= w::kEntityFlagDead;
		entity->engine_flags |= w::kEntityFlagDead;
		conn.link.respawn_pending = true;
		conn.link.respawn_delay_seconds = 0;
		conn.link.spawn_target_hold_seconds = 0;
	};
	auto dispatch = [&](uint16_t pick) {
		return inmatch::dispatch_session_replies(config, conn,
				{make_protocol_message(c2s::RESPAWN_REQUEST,
						{static_cast<uint8_t>(pick), static_cast<uint8_t>(pick >> 8)})},
				100, roster, &world);
	};
	const w::Entity *body = world.registry.get(player);
	// Slot team 1, entity team 2: the frontier is team 1's zone, which the resolve refuses.
	reset_player(2);
	if (!expect(dispatch(w::kDeployPickAutoTeam).empty() && body->health == 0,
			"the auto pick looks up the slot team's zone and the entity team refuses it"))
		return false;
	// A teamless entity passes the resolve: the slot team's zone deploys.
	reset_player(0);
	(void)dispatch(w::kDeployPickAutoTeam);
	if (!expect(body->health > 0 && body->position.x == 50.0f,
			"a teamless entity's auto pick deploys at the slot team's zone"))
		return false;
	// The numbered zone's team match reads the slot team.
	reset_player(0);
	(void)dispatch(numbered.packed);
	if (!expect(body->health > 0, "a picked numbered zone matches the slot team"))
		return false;
	// `nodefaultspawnpoints` asks for the slot team's zones, not the entity team's.
	config.default_spawn_requires_no_team_zone = 1;
	world.registry.get(other)->zone_number = 2;
	world.registry.get(other)->zone_control = 0;
	reset_player(2);
	return expect(dispatch(w::kDeployPickNone).empty() && body->health == 0,
			"Default Spawn is refused while the slot team holds a zone");
}

// D-NET-581: the spawn-wave list's rows hold player slots and read the slot
// team (+416): the picked row's team is matched against it, a miss refusing a
// pick whose zone has a row, and the S2C 0x6E counts and lists only its rows.
// [orig: SpawnWaveList_TryQueuePlayer @0x52A4BF, the compare @0x52A4C5..0x52A4C8,
//  the call @0x519D8F, SpawnWaveList_HasEntryForZone @0x519DAE;
//  NetPacket_WriteSpawnWaveStatus @0x5074CF, the row filter @0x507539]
bool check_split_slot_team_wave_rows() {
	w::World world;
	world.registry.configure_pool(0, 4);
	world.registry.configure_pool(2, 4);
	w::Entity player_seed;
	player_seed.kind = w::EntityKind::Organic;
	player_seed.flags = w::kEntityFlagPlayer | w::kEntityFlagDead;
	player_seed.engine_flags = player_seed.flags;
	player_seed.health = 0;
	player_seed.health_max = 100;
	const w::EntityHandle player = world.registry.spawn(0, player_seed);
	auto make_zone = [&](uint8_t team) { // an unnumbered zone, which the wave list rows
		w::Entity z;
		z.kind = w::EntityKind::Item;
		z.team = team;
		z.alive = true;
		z.is_spawn_point = true;
		z.zone_control = 0x10000;
		return world.registry.spawn(2, z);
	};
	const w::EntityHandle blue = make_zone(1);
	const w::EntityHandle red = make_zone(2);
	world.zones.spawn_waves.build_from_mission(world, 5, 5);
	inmatch::GameConfig config;
	config.game_type = game_type::kTeamDeathmatch;
	std::vector<inmatch::NapiNPConnection> roster(1);
	auto &conn = roster.front();
	conn.type = 1;
	conn.phase = inmatch::ConnectionPhase::InMatch;
	conn.burst.spawned = true;
	conn.link.owned_entity = player;
	conn.assigned_team = 1;
	conn.assigned_team_valid = true;
	auto pick = [&](uint8_t entity_team, w::EntityHandle zone) {
		w::Entity *entity = world.registry.get(player);
		entity->team = entity_team;
		conn.link.respawn_pending = true;
		return inmatch::dispatch_session_replies(config, conn,
				{make_protocol_message(c2s::RESPAWN_REQUEST,
						{static_cast<uint8_t>(zone.packed), static_cast<uint8_t>(zone.packed >> 8)})},
				100, roster, &world);
	};
	auto queued_at = [&](w::EntityHandle zone) {
		for (const w::SpawnWaveEntry &entry : world.zones.spawn_waves.entries())
			if (entry.zone == zone)
				return std::find(entry.queued.begin(), entry.queued.end(), player) !=
						entry.queued.end();
		return false;
	};
	// A teamless entity resolves the slot team's zone; its row matches the slot team.
	const std::vector<ProtocolMessage> queued = pick(0, blue);
	if (!expect(queued.size() == 1 && queued[0].tag == s2c::SPAWN_WAVE_STATUS &&
					queued_at(blue) && queued[0].payload.size() >= 3 &&
					queued[0].payload[0] == 1 &&
					queued[0].payload[1] == static_cast<uint8_t>(blue.packed) &&
					queued[0].payload[2] == static_cast<uint8_t>(blue.packed >> 8),
			"a teamless entity queues at the slot team's row and the 0x6E lists that row"))
		return false;
	// Entity team 2 resolves its own team's zone, whose row misses the slot team: refused.
	if (!expect(pick(2, red).empty() && !queued_at(red) && !queued_at(blue),
			"the other team's row refuses the slot and evicts it everywhere"))
		return false;
	// The 0x6E lists the slot team's rows whatever the entity's team.
	const std::vector<uint8_t> status = inmatch::build_spawn_wave_status_body(world, conn);
	return expect(status.size() >= 3 && status[0] == 1 &&
					status[1] == static_cast<uint8_t>(blue.packed) &&
					status[2] == static_cast<uint8_t>(blue.packed >> 8),
			"the 0x6E counts and lists the slot team's rows");
}

// D-NET-582: the deploy placement's no-pick arm positions as the slot team
// (+416), not the entity's, so a split slot's Default Spawn takes the slot
// team's Deaths gate and start marker. [orig: Server_PositionPlayerForSpawn
//  @0x50CF86..0x50CF92, the slot team @0x50D183, the Deaths gate @0x50D1DB,
//  the team markers @0x50D266]
bool check_split_slot_team_default_spawn_pose() {
	w::World world;
	world.registry.configure_pool(0, 4);
	world.registry.configure_pool(3, 4);
	for (const int32_t item_id : {6096, 6097}) { // team 1's start marker, team 2's
		w::Entity start;
		start.kind = w::EntityKind::Marker;
		start.item_id = item_id;
		start.position = {item_id == 6096 ? 100.0f : 200.0f, 0.0f, 0.0f};
		world.registry.spawn(3, start);
	}
	w::Entity player_seed;
	player_seed.kind = w::EntityKind::Organic;
	player_seed.flags = w::kEntityFlagPlayer | w::kEntityFlagDead;
	player_seed.engine_flags = player_seed.flags;
	player_seed.team = 2; // a script team write split the body from its slot
	player_seed.health = 0;
	player_seed.health_max = 100;
	const w::EntityHandle player = world.registry.spawn(0, player_seed);
	inmatch::GameConfig config;
	config.game_type = game_type::kTeamDeathmatch;
	std::vector<inmatch::NapiNPConnection> roster(1);
	auto &conn = roster.front();
	conn.type = 1;
	conn.phase = inmatch::ConnectionPhase::InMatch;
	conn.burst.spawned = true;
	conn.link.owned_entity = player;
	conn.link.respawn_pending = true;
	conn.assigned_team = 1;
	conn.assigned_team_valid = true;
	(void)inmatch::dispatch_session_replies(config, conn,
			{make_protocol_message(c2s::RESPAWN_REQUEST, {0xFF, 0xFF})}, 100, roster, &world);
	const w::Entity *body = world.registry.get(player);
	return expect(body->health > 0 && body->position.x == 100.0f,
			"a split slot's Default Spawn lands on the slot team's start marker");
}

bool check_shared_loaded_ammo_fire_and_reload() {
    w::World world;
    world.rules.mp_session = true;
    world.registry.configure_pool(0, 8);
    install_rifle_armory(world);
    auto &table = world.tables.weapons;
    table.ammo_class_names.push_back("SHARED");
    table.ammo_class_caps.push_back(1000);
    table.entries[5].ammo_bucket = 1;
    auto variant = table.entries[5]; variant.rank = 3;
    table.entries.push_back(variant);
    const auto shooter = w::spawn_remote_player(world, player_spawn(0,0,0));
    std::vector<inmatch::NapiNPConnection> roster;
    roster.push_back(make_seeded_conn(3,1,nullptr,ns::TransportMode::Client,shooter,true));
    auto &conn = roster.front();
    conn.reply.shared_clips[1] = 2;
    conn.reply.ammo_pools[0] = 5;
    for (uint8_t adm : {5,6}) {
        auto &slot = slots_of(world, conn)[3*65+table.entries[adm].rank];
        slot.adm_index = adm; slot.clip = 123;
    }
    uint32_t client_tick = conn.tick_seed;
    auto fire = [&](uint8_t adm) {
        (void)inmatch::dispatch_session_replies(inmatch::GameConfig{},conn,
            {make_protocol_message(c2s::FIRED_ROUND,fire_body(conn,shooter.packed,adm,client_tick))},100,roster,&world);
    };
    fire(5); fire(6); fire(5);
    if (!expect(conn.reply.shared_clips[1]==0 && world.out.rounds.count==2,
            "two weapon selections consume one shared clip; the third fire is empty")) return false;
    WeaponReload reload;
    reload.entity_handle=shooter.packed; reload.reload_param=3*65+3;
    (void)inmatch::dispatch_session_replies(inmatch::GameConfig{},conn,
        {make_protocol_message(c2s::WEAPON_RELOAD_REQUEST,encode_weapon_reload(reload))},100,roster,&world);
    if (!expect(conn.reply.shared_clips[1]==5 && conn.reply.ammo_pools[0]==0,
            "reload fills the shared clip from its reserve pool")) return false;
    fire(5);
    return expect(conn.reply.shared_clips[1]==4 && world.out.rounds.count==3 &&
            slots_of(world, conn)[197].clip==123 && slots_of(world, conn)[198].clip==123,
            "the other weapon sees the reload without rewriting either private clip");
}

// A validated primary round scores one FIRE event (field 2 plus the FIRE
// value); a rejected round and an alt-fire round score nothing.
// [orig: Server_ClientFiredRound @0x50BAA0 — the alt return @0x50BB0D, the
//  event-1 call @0x50C727]
bool check_validated_fire_scores_one_shot() {
	w::World world;
	world.rules.mp_session = true;
	world.registry.configure_pool(0, 8);
	install_rifle_armory(world);
	w::MatchRules rules;
	rules.game_type = game_type::kTeamDeathmatch;
	rules.score_values.emplace();
	(*rules.score_values)[0] = 3; // FIRE
	world.match.configure(rules);
	const w::EntityHandle shooter = w::spawn_remote_player(world, player_spawn(0, 0, 0));
	const w::EntityHandle other = w::spawn_remote_player(world, player_spawn(1, 0, 0));
	world.match.upsert_player({shooter, 3, "Shooter"});
	std::vector<inmatch::NapiNPConnection> roster;
	roster.push_back(make_seeded_conn(3, 1, nullptr, ns::TransportMode::Client, shooter, true));
	roster.push_back(make_seeded_conn(4, 1, nullptr, ns::TransportMode::Client, other, true));
	inmatch::NapiNPConnection &conn = roster[0];
	slots_of(world, conn)[3 * 65 + 2] = w::PersonalWeaponSlot{5, 30}; // the grant's rifle
	uint32_t client_tick = conn.tick_seed;
	auto dispatch_fire = [&](std::vector<uint8_t> body) {
		std::vector<ProtocolMessage> msgs;
		msgs.push_back(make_protocol_message(c2s::FIRED_ROUND, std::move(body)));
		(void)inmatch::dispatch_session_replies(inmatch::GameConfig{}, conn, msgs, 100, roster, &world);
	};
	const w::MatchPlayer *row = world.match.player(shooter);
	dispatch_fire(fire_body(conn, other.packed, 5, client_tick)); // spoofed: rejected
	if (!expect(row->stats[w::MatchStats::kShotsFired] == 0, "a rejected round scores no shot"))
		return false;
	dispatch_fire(fire_body(conn, shooter.packed, 5, client_tick));
	if (!expect(world.out.rounds.count == 1 && row->stats[w::MatchStats::kShotsFired] == 1 &&
					row->stats[w::MatchStats::kPoints] == 3 &&
					world.match.team_stats(1)[w::MatchStats::kShotsFired] == 1,
			"the validated round scores one FIRE event"))
		return false;
	ClientFiredRound alt;
	alt.current_tick = ++client_tick;
	alt.shooter_handle = shooter.packed;
	alt.target_handle = 0xFFFF;
	alt.adm_index = 5;
	alt.fire_flags = 0x23; // bit 0: alt fire
	dispatch_fire(encode_client_fired_round(alt));
	return expect(world.out.rounds.count == 2 && row->stats[w::MatchStats::kShotsFired] == 1,
			"an alt-fire round is appended without a FIRE event");
}

// The host's record of a remote player's EquippedSlot: the 0x2F accept points it at
// the submitted slot and clears it for an empty slot or a NoSelect def; an accepted network fire
// re-points it at the fired combo's personal slot; the 0x0C weapon echo never moves it.
// [orig: NapiNPServerMsg_HandlePlayerLoadout @0x515f52..0x515f8a;
//  Server_ClientFiredRound @0x50c1d4..0x50c28d]
bool check_host_tracks_the_remote_equipped_slot() {
	w::World world;
	world.rules.mp_session = true;
	world.registry.configure_pool(0, 8);
	install_rifle_armory(world);
	w::WeaponTableEntry &selectable = world.tables.weapons.entries[4];
	selectable = world.tables.weapons.entries[5];
	selectable.name = "WPN_TESTCHUTE";
	selectable.rank = 1;
	selectable.flags2 = w::weapon_flag2::kNoSelect;
	selectable.ammo_class = "CLS_B"; // its own pool: the rifle keeps its clip
	selectable.ammo_class_id = 1;
	world.tables.weapons.ammo_class_names.push_back("CLS_B");
	world.tables.weapons.ammo_class_caps.push_back(1000);
	const w::EntityHandle shooter = w::spawn_remote_player(world, player_spawn(0, 0, 0));
	std::vector<inmatch::NapiNPConnection> roster;
	roster.push_back(make_seeded_conn(3, 1, nullptr, ns::TransportMode::Client, shooter, true));
	inmatch::NapiNPConnection &conn = roster.front();
	const uint16_t rifle_combo = 3 * 65 + 2;
	const uint16_t chute_combo = 3 * 65 + 1;
	using Ref = w::WeaponSlotRef;
	const auto equipped = [&]() { return world.registry.get(shooter)->equipped_slot; };
	auto submit = [&](uint32_t slot) {
		LoadoutSubmit request;
		request.team = 1;
		request.player_class = 8;
		request.weapon_slot_index = slot;
		request.entries.push_back(LoadoutSubmitEntry{5, 3, 0xFF, 0xFF});
		request.entries.push_back(LoadoutSubmitEntry{4, 1, 0xFF, 0xFF});
		conn.link.armory_reuse_seconds = 0;
		conn.link.preround_loadout_latch = true; // the post-join submit bypasses the window
		(void)inmatch::dispatch_session_replies(inmatch::GameConfig{}, conn,
				{make_protocol_message(c2s::LOADOUT_SUBMIT, encode_loadout_submit(request))},
				100, roster, &world);
	};
	submit(rifle_combo);
	if (!expect(slots_of(world, conn).count(rifle_combo) == 1 && equipped().kind == Ref::kPersonal &&
					equipped().combo == rifle_combo,
			"a selectable submitted slot is the EquippedSlot"))
		return false;
	submit(3 * 65 + 9);
	if (!expect(equipped().kind == Ref::kNone, "an empty submitted slot clears it"))
		return false;
	submit(chute_combo);
	if (!expect(equipped().kind == Ref::kNone, "a NoSelect submitted slot clears it"))
		return false;
	uint32_t client_tick = conn.tick_seed;
	// A gun's slot held on foot (a refused UseGun attach, D-NET-420) is no seat:
	// the fire takes the personal slot and re-points to it.
	world.registry.get(shooter)->equipped_slot.kind = Ref::kMount;
	world.registry.get(shooter)->equipped_slot.mount = shooter;
	(void)inmatch::dispatch_session_replies(inmatch::GameConfig{}, conn,
			{make_protocol_message(c2s::FIRED_ROUND, fire_body(conn, shooter.packed, 5, client_tick))},
			100, roster, &world);
	return expect(world.out.rounds.count == 1 && equipped().kind == Ref::kPersonal &&
					equipped().combo == rifle_combo,
			"an accepted fire re-points the EquippedSlot at the fired combo");
}

bool check_guidance_waits_for_spawn_frame() {
    inmatch::NapiNPServerCtx ctx;
    inmatch::set_connection_mode(ctx,inmatch::ConnectionMode::HostOnly);
    ctx.is_in_session=1;
    ctx.network_quality_broadcast_countdown=100;
    ctx.config.game_type=game_type::kTeamDeathmatch;
    w::World world;
    world.rules.mp_session=true;
    world.registry.configure_pool(0,8);
    w::MatchRules rules; rules.game_type=ctx.config.game_type; world.match.configure(rules);
    ctx.world=&world;
    const auto player=w::spawn_remote_player(world,player_spawn(0,0,0));
    ns::UdpSessionTransport transport(ns::UdpSessionTransport::Role::Host);
    ctx.np_protocol.connection_list.push_back(make_seeded_conn(3,1,&transport,ns::TransportMode::Client,player,true));
    auto &conn=ctx.np_protocol.connection_list.front();
    conn.s2c_send_boundary_open=false;
    w::RoundSim::GuidedUpdate update{};
    update.shooter=player.packed; update.net_id=9; update.groups=1u<<3;
    update.state.target=0x1001; update.state.steer[0]=1234;
    world.round_sim.guided_updates.push_back(update);
    inmatch::Server_TickUpdate(ctx);
    for (const auto &d:drain(transport))
        if (!expect(d.tag!=0x44,"guidance waits while the spawn frame boundary is closed")) return false;
    if (!expect(conn.pending_guidance.size()==1,"guidance survives the closed send boundary")) return false;
    conn.s2c_send_boundary_open=true;
    inmatch::Server_TickUpdate(ctx);
    bool frame=false, guidance=false;
    for (const auto &d:drain(transport)) {
        if (d.tag==0x0A) frame=true;
        if (d.tag==0x44) {
            if (!expect(frame,"the fire-bearing frame precedes queued guidance")) return false;
            guidance=true;
        }
    }
    return expect(guidance && conn.pending_guidance.empty(),"queued guidance drains once at the open boundary");
}

} // namespace

int main() {
	bool ok = check_shared_loaded_ammo_fire_and_reload() && check_guidance_waits_for_spawn_frame();
	ok = check_validated_fire_scores_one_shot() && ok;
	ok = check_host_tracks_the_remote_equipped_slot() && ok;
	ok = check_spawn_protection_seeded_at_creation() && ok;
	ok = check_spawn_protection_countdown_and_gates() && ok;
	ok = check_spawn_protection_cleared_by_validated_fire() && ok;
	ok = check_deploy_seeds_protection_and_armory_state() && ok;
	ok = check_flag_refresh_round_robin() && ok;
	ok = check_humans_count_the_visible_players() && ok;
	ok = check_recount_precedes_the_entity_update() && ok;
	ok = check_server_tick_leaves_the_pending_sounds() && ok;
	ok = check_flag_carry_limit_breaks_the_carry_and_kills(game_type::kCaptureTheFlag, true) && ok;
	ok = check_flag_carry_limit_breaks_the_carry_and_kills(game_type::kTeamDeathmatch, false) && ok;
	ok = check_flag_carry_limit_breaks_the_carry_and_kills(game_type::kCaptureTheFlag, true,
			/*uplinked=*/true) && ok;
	ok = check_flag_reset_dies_ahead_of_the_next_pump(c2s::TEAM_CHANGE_REQUEST, false) && ok;
	ok = check_flag_reset_dies_ahead_of_the_next_pump(c2s::DEATH_TIMEOUT_RESET, false) && ok;
	ok = check_flag_reset_dies_ahead_of_the_next_pump(c2s::TEAM_CHANGE_REQUEST, true) && ok;
	ok = check_flag_reset_dies_ahead_of_the_next_pump(c2s::DEATH_TIMEOUT_RESET, true) && ok;
	ok = check_flag_reset_waiting_edge_skips_a_dead_body() && ok;
	ok = check_flag_reset_credits_a_staged_attacker(true) && ok;
	ok = check_flag_reset_credits_a_staged_attacker(false) && ok;
	ok = check_team_downed_resend() && ok;
	ok = check_death_consumer_reads_and_clears_the_reported_cause_bit() && ok;
	ok = check_spectator_latch_mirrors_onto_the_roster_row() && ok;
	ok = check_armory_reuse_cooldown() && ok;
	ok = check_model_page_cursor_wraps_at_the_list_count() && ok;
	ok = check_quartet_skips_a_spectator_slot() && ok;
	ok = check_silence_punt_falls_through_to_the_countdown() && ok;
	ok = check_integrity_family_draws_once_per_send() && ok;
	ok = check_entity_state_fan_runs_out_of_session() && ok;
	ok = check_a_loading_joiner_takes_no_proximity_or_census_place() && ok;
	ok = check_slot_team_readers_ignore_a_body_team_write() && ok;
	ok = check_host_quality_samples_ahead_of_the_receive_pump() && ok;
	ok = check_designator_round_marks_and_notifies_the_team() && ok;
	ok = check_zone_timer_every_pass_and_wave_status_by_the_dead_bit() && ok;
	ok = check_priority_target_sweep() && ok;
	ok = check_kit_weight_recompute() && ok;
	ok = check_kit_weight_reads_the_seat() && ok;
	ok = check_deploy_rebuilds_the_kit_from_the_loadout_buffer() && ok;
	ok = check_deploy_rebuilds_an_unsubmitted_slot_empty() && ok;
	ok = check_deploy_leaves_the_listen_hosts_kit() && ok;
	ok = check_deploy_pools_follow_the_buffered_name() && ok;
	ok = check_round_init_rebuilds_the_kept_kit() && ok;
	ok = check_loadout_damage_class_bytes() && ok;
	ok = check_loadout_sub_variant_rows() && ok;
	ok = check_sub_variant_fire_after_the_grant() && ok;
	ok = check_try_queue_evicts_on_failed_pick() && ok;
	ok = check_respawn_pick_admission() && ok;
	ok = check_revived_pick_takes_no_target() && ok;
	ok = check_spectator_pick_legs() && ok;
	ok = check_split_slot_team_pick_legs() && ok;
	ok = check_split_slot_team_default_spawn_pose() && ok;
	ok = check_split_slot_team_wave_rows() && ok;
	if (ok) std::printf("OK\n");
	return ok ? 0 : 1;
}
