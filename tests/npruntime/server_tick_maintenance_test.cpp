// Server_TickUpdate's per-tick and periodic-second maintenance legs ported from the
// jo-c cross-check (2026-09-12):
//   - the 620-tick spawn protection (entity+292): seeded at creation, join and every
//     deploy, counted down per authority tick in a live MP session, frozen during
//     pre-round and after round end, zeroed outside a session, held at -1 for a
//     spectator, and cleared by the first validated fire
//     [orig: Server_UpdateAllActivePlayerSlots @0x518820; Server_ProcessPlayerDeath
//      @0x517740; Server_ClientFiredRound @0x50c736..0x50c75d];
//   - the 1 Hz round-robin S2C 0x2F flag refresh [orig: sub_517B20 @0x517B20];
//   - the violation sweep's flag carry limit [orig: Server_CheckPlayerViolations
//     @0x51ABD0 -> Entity_DropCarriedObject @0x439DF0 + Entity_SyncPositionFromDefinition
//     @0x43A9B0] (the t6 punt arms are pinned in host_punt_test);
//   - the team-mode 1 Hz S2C 0x46 field-0x0008 downed resend [orig: Server_TickUpdate
//     @0x51E2D0..0x51E378];
//   - the death consumer's live entity+44 cause read and reported-bit clear
//     [orig: GameEvent_PlayerDeath @0x516DD0 — @0x516f4d, @0x517180..0x517206];
//   - the roster row's spectator latch mirror [orig: Server_PlayerAdd @0x51CD83;
//     Server_KillPlayerAndNotify @0x519E76];
//   - the armory-reuse cooldown and pre-round latch around C2S 0x2F
//     [orig: NapiNPServerMsg_HandlePlayerLoadout @0x515790];
//   - the 744-tick priority-target sweep [orig: EntityPool_ClearDirtyFlags @0x508E30];
//   - the 1 Hz / accept-time kit-weight recompute [orig: recalculate_all_player_scores
//     @0x5014E0];
//   - the unconditional other-row eviction of SpawnWaveList_TryQueuePlayer @0x52A490;
//   - the C2S 0x0E Conquer & Control auto-pick rejection and the handle-0 admission
//     [orig: Server_ProcessClientRequestRespawn @0x519AF0].

#include <runtime/inmatch/napi_np_connection.h>
#include <runtime/inmatch/napi_np_protocol.h>
#include <runtime/inmatch/napi_np_server_ctx.h>
#include <runtime/inmatch/server_message_dispatch.h>
#include <runtime/inmatch/server_session.h>
#include <runtime/inmatch/server_spawn.h>
#include <runtime/inmatch/server_tick.h>

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

#include <cstdint>
#include <cstdio>
#include <vector>

#include "conn_fixture.h"
#include "host_test_setup.h"

namespace {

using namespace opennova;
namespace inmatch = opennova::inmatch;
namespace ns = opennova::replication;
namespace w = opennova::world;

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

inmatch::NapiNPConnection make_conn(uint32_t id, int type,
		ns::ISessionTransport *transport, ns::TransportMode mode,
		w::EntityHandle owned, bool spawned) {
	auto connection = conn_fixture::make_conn(id, type, transport, mode, owned, spawned);
	if (spawned) (void)inmatch::Server_RerollPlayerTickSeed(connection);
	return connection;
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
	// A remote joiner: 620 at creation regardless of the spawn-zone hold.
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
		const w::EntityHandle player = inmatch::Server_BuildPlayerInfoAndAdd(ctx, live, world);
		const w::Entity *entity = world.registry.get(player);
		if (!expect(entity != nullptr && entity->damage_state == 620,
				"a zone-less joiner's entity is created under the 620-tick protection"))
			return false;
		if (!expect(live.link.armory_reuse_seconds == 0 && !live.link.preround_loadout_latch,
				"the join zeroes the armory cooldown and clears the latch outside pre-round"))
			return false;
	}
	// The listen host's own player takes the same creation seed.
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
		const w::Entity *host = world.registry.get(world.cached.local_player);
		if (!expect(host != nullptr && host->damage_state == 620,
				"the host's own player is created under the 620-tick protection"))
			return false;
		return expect(ctx.np_protocol.connection_list.front().link.preround_loadout_latch,
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
			make_conn(3, 1, &transport, ns::TransportMode::Client, player, true));
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
	roster.push_back(make_conn(3, 1, nullptr, ns::TransportMode::Client, shooter, true));
	roster.push_back(make_conn(4, 1, nullptr, ns::TransportMode::Client, other, true));
	inmatch::NapiNPConnection &conn = roster[0];
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
	dispatch_fire(fire_body(conn, shooter.packed, 5, client_tick));
	if (!expect(world.out.rounds.count == 1 && entity->damage_state == 0,
			"the first validated fire clears the protection"))
		return false;
	// Outside a session the clear is not the fire's concern (the tick arm owns it).
	world.rules.mp_session = false;
	entity->damage_state = 77;
	dispatch_fire(fire_body(conn, shooter.packed, 5, client_tick));
	return expect(world.out.rounds.count == 2 && entity->damage_state == 77,
			"the fire clear is gated on the live session");
}

bool check_deploy_seeds_protection_and_armory_state() {
	w::World world;
	world.rules.mp_session = true;
	world.registry.configure_pool(0, 8);
	world.registry.configure_pool(3, 8);
	const w::EntityHandle player = w::spawn_remote_player(world, player_spawn(0, 0, 0));
	inmatch::NapiNPConnection conn =
			make_conn(3, 1, nullptr, ns::TransportMode::Client, player, true);
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
			inmatch::Server_ReleasePlayerDeployment(config, conn, world, {});
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
	(void)inmatch::Server_ReleasePlayerDeployment(config, conn, world, {});
	return expect(!conn.link.preround_loadout_latch,
			"a deploy outside pre-round leaves the latch clear");
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
			make_conn(3, 1, &remote, ns::TransportMode::Client, player, true));
	const w::EntityHandle host = w::spawn_player(world, player_spawn(0, 0, 0));
	ctx.np_protocol.connection_list.push_back(
			make_conn(1, 2, &host_wire, ns::TransportMode::Loopback, host, true));

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
		bool expect_kill) {
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
			make_conn(3, 1, &remote, ns::TransportMode::Client, carrier, true));
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
	// The same second's refresh already publishes the home pose; the drop and
	// sync records queued by the sweep are drained on the following tick.
	if (!expect(objective_states(limit_second).size() == 1 && !saw_flag_return_feed(limit_second),
			"the limit second carries the refresh 0x2F and no 0x1E return event"))
		return false;
	inmatch::Server_TickUpdate(ctx);
	const std::vector<ns::Datagram> next_tick = drain(remote);
	const std::vector<ObjectiveEntityState> pair = objective_states(next_tick);
	if (!expect(pair.size() == 2 && pair[0].entity_handle == flag.packed &&
			pair[0].attach_handle == 0xFFFF && pair[0].pos_x == w::to_fixed(carrier_position.x) &&
			pair[0].pos_y == w::to_fixed(carrier_position.y) &&
			pair[1].entity_handle == flag.packed && pair[1].attach_handle == 0xFFFF &&
			pair[1].pos_x == 5 * 65536 && pair[1].pos_y == 6 * 65536,
			"the carry break fans the drop-pose 0x2F then the home-pose 0x2F"))
		return false;
	if (!expect(!saw_flag_return_feed(next_tick), "the carry break never emits a 0x23..0x25 return feed"))
		return false;
	bool saw_death = false;
	for (const ns::Datagram &d : next_tick)
		if (d.tag == s2c::ENTITY_DEATH && d.body.size() == 4 &&
				(d.body[0] | (d.body[1] << 8)) == carrier.packed)
			saw_death = true;
	return expect(saw_death && !body->alive && (body->flags & w::kEntityFlagDead) != 0,
			"the carrier dies through the ordinary death transaction");
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
	roster.push_back(make_conn(3, 1, &victim_wire, ns::TransportMode::Client, victim, true));
	roster.push_back(make_conn(4, 1, &killer_wire, ns::TransportMode::Client, killer, true));
	roster.push_back(make_conn(5, 1, &mate_wire, ns::TransportMode::Client, teammate, true));
	roster[0].reply.player_slot = 1;
	roster[1].reply.player_slot = 2;
	roster[2].reply.player_slot = 3;

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

	// The revivable kill: another player, no knife/headshot cause bits. The round
	// sim zeroes Health before it raises the death; the routing marks the body dead.
	world.registry.get(victim)->health = 0;
	w::RoundDeath death;
	death.victim = victim;
	death.killer = killer;
	death.victim_handle = victim.packed;
	death.killer_handle = killer.packed;
	world.round_sim.deaths.push_back(death);
	// Tick 1: the death routes (window 120), the shared periodic second already
	// decrements it to 119, then the zero-armed resend countdown fires.
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
	roster.push_back(make_conn(3, 1, &head_wire, ns::TransportMode::Client, head, true));
	roster.push_back(make_conn(4, 1, &multi_wire, ns::TransportMode::Client, multi, true));
	roster.push_back(make_conn(5, 1, &self_wire, ns::TransportMode::Client, self, true));
	roster.push_back(make_conn(6, 1, &killer_wire, ns::TransportMode::Client, killer, true));
	roster[0].reply.player_slot = 1;
	roster[1].reply.player_slot = 2;
	roster[2].reply.player_slot = 3;
	roster[3].reply.player_slot = 4;

	auto raise_death = [&](w::EntityHandle victim, w::EntityHandle by, uint32_t cause) {
		w::Entity *body = world.registry.get(victim);
		body->health = 0;
		body->cause_flags = cause;
		// The lethal hit re-armed the plyr think (spawnPhase = 64), so the
		// cadence clear cannot race this tick's consumer.
		body->spawn_phase = 64;
		w::RoundDeath death;
		death.victim = victim;
		death.killer = by;
		death.victim_handle = victim.packed;
		death.killer_handle = by.packed;
		world.round_sim.deaths.push_back(death);
	};

	// A critical (head-zone) kill: the 0x1E ships the STRCND08 family 10..12,
	// the reported 0x800 is cleared on the body, and no revive window opens.
	raise_death(head, killer, 0x800u);
	inmatch::Server_TickUpdate(ctx);
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
	inmatch::Server_TickUpdate(ctx);
	feed = death_feed_types(killer_wire);
	if (!expect(feed.size() == 1 && feed[0] == 32 &&
			world.registry.get(multi)->cause_flags == 0x800u,
			"a latched 0x100 reports 32 and clears only its own bit"))
		return false;

	// The suicide branch returns before the ladder, so nothing is cleared.
	raise_death(self, self, 0x800u);
	inmatch::Server_TickUpdate(ctx);
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
// skipped [orig: Server_KillPlayerAndNotify @0x519E76; Match::finish].
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
	roster.push_back(make_conn(3, 1, &wire, ns::TransportMode::Client, player, true));
	roster[0].phase = inmatch::ConnectionPhase::InMatch;
	roster[0].reply.player_slot = 1;

	if (!expect(world.match.player(player) != nullptr &&
			!world.match.player(player)->spectator,
			"a joined player's roster row starts non-spectator"))
		return false;
	if (!expect(inmatch::Server_SetPlayerSpectator(ctx, roster[0], world, true) &&
			roster[0].link.spectator && world.match.player(player)->spectator,
			"the runtime spectator convert mirrors the latch onto the roster row"))
		return false;
	(void)inmatch::Server_SetPlayerSpectator(ctx, roster[0], world, false);
	return expect(!roster[0].link.spectator && !world.match.player(player)->spectator,
			"leaving spectator mode clears the roster latch");
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
	roster.push_back(make_conn(3, 1, nullptr, ns::TransportMode::Client, player, true));
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
	// A spectator's empty grant never seeds.
	conn.link.spectator = true;
	conn.link.armory_reuse_seconds = 0;
	replies = dispatch(submit);
	return expect(replies.size() == 1 && conn.link.armory_reuse_seconds == 0,
			"a spectator submit seeds no cooldown");
}

// --------------------------------------------------------------------------
// The 744-tick priority-target sweep.
// --------------------------------------------------------------------------

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
	roster.push_back(make_conn(1, 2, &host_wire, ns::TransportMode::Loopback, host, true));
	roster.push_back(make_conn(3, 1, &remote_wire, ns::TransportMode::Client, remote, true));
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
			remote_conn.weapon_slots[combo].clip == 30 && remote_weight() == 0x60000,
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
	if (!expect(remote_conn.weapon_slots[combo].clip == 0 &&
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
	if (!expect(remote_conn.weapon_slots[combo].clip == 30 && remote_conn.reply.ammo_pools[0] == 30,
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
	if (!expect(waves.try_queue(world, zone_a, p0) && holds(zone_a, p0),
			"the first pick queues at zone A"))
		return false;
	for (int i = 0; i < 8; ++i) {
		if (!expect(waves.try_queue(world, zone_b, make_player(1)), "zone B fills to eight"))
			return false;
	}
	if (!expect(!waves.try_queue(world, zone_b, p0) && !holds(zone_a, p0) && !holds(zone_b, p0),
			"a full-row pick fails AND evicts the player from its previous zone"))
		return false;
	if (!expect(waves.try_queue(world, zone_a, p0) && holds(zone_a, p0),
			"the player re-queues at zone A"))
		return false;
	if (!expect(!waves.try_queue(world, zone_c, p0) && !holds(zone_a, p0) && !holds(zone_c, p0),
			"an other-team zone pick fails AND evicts the player everywhere"))
		return false;
	if (!expect(waves.try_queue(world, zone_a, p0) && !waves.try_queue(world, zone_a, p0) &&
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

} // namespace

int main() {
	bool ok = true;
	ok = check_spawn_protection_seeded_at_creation() && ok;
	ok = check_spawn_protection_countdown_and_gates() && ok;
	ok = check_spawn_protection_cleared_by_validated_fire() && ok;
	ok = check_deploy_seeds_protection_and_armory_state() && ok;
	ok = check_flag_refresh_round_robin() && ok;
	ok = check_flag_carry_limit_breaks_the_carry_and_kills(game_type::kCaptureTheFlag, true) && ok;
	ok = check_flag_carry_limit_breaks_the_carry_and_kills(game_type::kTeamDeathmatch, false) && ok;
	ok = check_team_downed_resend() && ok;
	ok = check_death_consumer_reads_and_clears_the_reported_cause_bit() && ok;
	ok = check_spectator_latch_mirrors_onto_the_roster_row() && ok;
	ok = check_armory_reuse_cooldown() && ok;
	ok = check_priority_target_sweep() && ok;
	ok = check_kit_weight_recompute() && ok;
	ok = check_try_queue_evicts_on_failed_pick() && ok;
	ok = check_respawn_pick_admission() && ok;
	if (ok) std::printf("OK\n");
	return ok ? 0 : 1;
}
