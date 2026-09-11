// The S2C 0x54 / 0x14 medic recipient set. Retail's send mask 0x580 is the
// conjunction of player-slot state 6/7 (in-match), the slot team byte and the
// Medic class attribute; no leg reads entity health or the Flags dead bit, and
// the ordinary death path never rewrites slot+0x20, so a dead or
// respawn-pending same-team Medic still receives the downed record on a
// teammate's death and the 0x54 + 0x14 pair on a medic request. The 0x34 help
// call keeps its separate mask-128 alive filter.
// [orig: GameEvent_PlayerDeath @0x51739C (mask 0x580) / @0x5173AF (team
//  filter); NapiNPServer_SendFiltered @0x4C87E0 — roster gate @0x4C889F,
//  0x80 @0x4C8948..0x4C8953, 0x100 @0x4C896A..0x4C8977,
//  0x400 @0x4C8990..0x4C89A8; Server_BroadcastMedicRequest @0x515390]
#include <runtime/inmatch/napi_np_connection.h>
#include <runtime/inmatch/napi_np_server_ctx.h>
#include <runtime/inmatch/server_message_dispatch.h>
#include <runtime/inmatch/server_tick.h>

#include <runtime/inmatch/loopback_channel.h>

#include <net/npwire/ingame_decode.h>
#include <net/npwire/ingame_encode.h>
#include <net/npwire/ingame_message_id.h>
#include <net/npwire/protocol_message.h>

#include <base/gameprofile/game_type.h>
#include <runtime/world/entity.h>
#include <runtime/world/world.h>

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "conn_fixture.h"

namespace {

using namespace opennova;
namespace inmatch = opennova::inmatch;
namespace ns = opennova::replication;
namespace w = opennova::world;

int failures = 0;
bool expect(bool cond, const char *msg) {
	if (cond) return true;
	std::fprintf(stderr, "FAIL: %s\n", msg);
	++failures;
	return false;
}

using conn_fixture::make_conn;

w::EntityHandle spawn_person(w::World &world, uint8_t team,
		uint8_t player_class, const char *name) {
	w::Entity e;
	e.kind = w::EntityKind::Organic;
	e.player_class = player_class;
	e.team = team;
	e.health = 100;
	e.alive = true;
	e.flags = w::kEntityFlagPlayer;
	e.engine_flags = w::kEntityFlagPlayer;
	e.name = name;
	return world.registry.spawn(0, e);
}

// Drain one channel into [tag][body...] rows.
std::vector<std::vector<uint8_t>> drain(ns::LoopbackChannel &channel) {
	std::vector<std::vector<uint8_t>> out;
	ns::Datagram datagram;
	while (channel.client_recv(datagram)) {
		std::vector<uint8_t> raw;
		raw.push_back(datagram.tag);
		raw.insert(raw.end(), datagram.body.begin(), datagram.body.end());
		out.push_back(std::move(raw));
	}
	return out;
}

std::vector<std::vector<uint8_t>> bodies_of(
		const std::vector<std::vector<uint8_t>> &rows, uint8_t tag) {
	std::vector<std::vector<uint8_t>> out;
	for (const std::vector<uint8_t> &row : rows)
		if (!row.empty() && row[0] == tag)
			out.emplace_back(row.begin() + 1, row.end());
	return out;
}

} // namespace

int main() {
	w::World world;
	world.registry.configure_pool(0, 8);
	world.rules.mp_session = true;
	// Class 8 carries the Medic charattr; class 7 does not.
	world.tables.class_attribute_flags[(8u - 1u) & 0xFu] |=
			w::MissionTables::kCharAttrMedic;

	const w::EntityHandle victim = spawn_person(world, 1, 7, "Downed");
	const w::EntityHandle dead_medic = spawn_person(world, 1, 8, "DeadMedic");
	const w::EntityHandle rifleman = spawn_person(world, 1, 7, "Rifleman");
	const w::EntityHandle enemy_medic = spawn_person(world, 2, 8, "EnemyMedic");
	// The medic is already dead when the teammate goes down: the exact state
	// route_round_deaths leaves on a victim (alive false, Flags bit 1, 0 hp).
	{
		w::Entity *medic = world.registry.get(dead_medic);
		medic->alive = false;
		medic->flags |= w::kEntityFlagDead;
		medic->health = 0;
		medic->damage_state = -1;
	}
	world.match.upsert_player({victim, 0, "Downed"});
	world.match.upsert_player({dead_medic, 1, "DeadMedic"});
	world.match.upsert_player({rifleman, 2, "Rifleman"});
	world.match.upsert_player({enemy_medic, 3, "EnemyMedic"});

	ns::LoopbackChannel victim_wire, medic_wire, rifleman_wire, enemy_wire;
	inmatch::NapiNPServerCtx ctx;
	ctx.world = &world;
	ctx.is_authority = 1;
	ctx.is_in_session = 1;
	ctx.config.game_type = game_type::kTeamDeathmatch;
	std::vector<inmatch::NapiNPConnection> &roster = ctx.np_protocol.connection_list;
	roster.push_back(make_conn(1, 1, &victim_wire, ns::TransportMode::Client, victim, true));
	roster.push_back(make_conn(2, 1, &medic_wire, ns::TransportMode::Client, dead_medic, true));
	roster.push_back(make_conn(3, 1, &rifleman_wire, ns::TransportMode::Client, rifleman, true));
	roster.push_back(make_conn(4, 1, &enemy_wire, ns::TransportMode::Client, enemy_medic, true));
	for (size_t i = 0; i < roster.size(); ++i) {
		roster[i].reply.player_slot = static_cast<uint8_t>(i);
		roster[i].admission_stage = inmatch::GameAdmissionStage::Complete;
		roster[i].link.last_deploy_tick = world.logic_tick;
		roster[i].link.last_deploy_tick_valid = true;
	}

	// --- The predicate itself: in-match + team byte + Medic class, nothing else.
	expect(inmatch::is_medic_recipient(roster[1], world, 1),
			"a dead same-team Medic is a mask-0x580 recipient (no health/dead test)");
	expect(!inmatch::is_medic_recipient(roster[2], world, 1),
			"a same-team non-Medic is filtered by the 0x400 class leg");
	expect(!inmatch::is_medic_recipient(roster[3], world, 1),
			"an enemy Medic is filtered by the 0x100 team leg");
	{
		inmatch::NapiNPConnection undeployed = make_conn(
				5, 1, &medic_wire, ns::TransportMode::Client, dead_medic, false);
		expect(!inmatch::is_medic_recipient(undeployed, world, 1),
				"a Medic outside slot state 6/7 is filtered by the 0x80 leg");
	}

	// --- The death fan: a teammate downed by an enemy opens the revive window
	// and the dead Medic receives the 0x54 record; the non-Medic and the
	// enemy Medic do not.
	inmatch::Server_TickUpdate(ctx); // first-frame one-second service
	drain(victim_wire);
	drain(medic_wire);
	drain(rifleman_wire);
	drain(enemy_wire);
	{
		w::RoundDeath d;
		d.victim = victim;
		d.killer = enemy_medic;
		d.victim_handle = victim.packed;
		d.killer_handle = enemy_medic.packed;
		world.round_sim.deaths.push_back(d);
	}
	inmatch::Server_TickUpdate(ctx);
	expect(roster[0].link.downed_revive_seconds == 120,
			"an other-player kill arms the 120-second revive window");
	{
		const auto medic_downed = bodies_of(drain(medic_wire), s2c::PLAYER_DOWNED_STATE);
		expect(medic_downed.size() == 1 && medic_downed[0].size() == 3 &&
						medic_downed[0][0] == uint8_t(victim.packed & 0xFFu) &&
						medic_downed[0][1] == uint8_t(victim.packed >> 8),
				"death: the dead same-team Medic receives the victim's 0x54 record");
		expect(bodies_of(drain(rifleman_wire), s2c::PLAYER_DOWNED_STATE).empty(),
				"death: a same-team non-Medic receives no 0x54");
		expect(bodies_of(drain(enemy_wire), s2c::PLAYER_DOWNED_STATE).empty(),
				"death: an enemy Medic receives no 0x54");
		drain(victim_wire);
	}

	// --- The medic request (C2S 0x2E) from the downed victim in manual mode:
	// the dead Medic receives the live window (0x54) and the STRSRV_MEDREQ
	// chat line (0x14) but not the mask-128 help call (0x34), which the alive
	// non-Medic still gets.
	{
		roster[0].link.auto_medic_enabled = false;
		roster[0].link.medic_request_active = false;
		const std::string format = "%s needs a medic!";
		inmatch::ServerDispatchInputs inputs;
		inputs.medic_request_format = &format;
		MedicRequest request;
		request.entity_index = victim.packed & 0xFFFu;
		const std::vector<ProtocolMessage> own = inmatch::dispatch_session_replies(
				ctx.config, roster[0],
				{make_protocol_message(c2s::MEDIC_REQUEST,
						encode_medic_request(request))},
				world.logic_tick, roster, &world, inputs);
		expect(own.size() == 1 && own[0].tag == s2c::CHAT_BROADCAST,
				"the requester gets the chat line back");
		const auto medic_rows = drain(medic_wire);
		const auto medic_downed = bodies_of(medic_rows, s2c::PLAYER_DOWNED_STATE);
		expect(medic_downed.size() == 1 &&
						medic_downed[0] == std::vector<uint8_t>({
								uint8_t(victim.packed & 0xFFu),
								uint8_t(victim.packed >> 8), 120}),
				"request: the dead Medic receives the live 120-second window without bit 7");
		expect(bodies_of(medic_rows, s2c::CHAT_BROADCAST).size() == 1,
				"request: the dead Medic receives the STRSRV_MEDREQ chat line");
		expect(bodies_of(medic_rows, 0x34).empty(),
				"request: the help call keeps its own alive filter (no 0x34 to a dead Medic)");
		const auto rifleman_rows = drain(rifleman_wire);
		expect(bodies_of(rifleman_rows, s2c::PLAYER_DOWNED_STATE).empty() &&
						bodies_of(rifleman_rows, s2c::CHAT_BROADCAST).empty(),
				"request: a same-team non-Medic receives neither the window nor the chat");
		expect(bodies_of(rifleman_rows, 0x34).size() == 1,
				"request: the alive non-Medic still receives the mask-128 help call");
		expect(bodies_of(drain(enemy_wire), s2c::PLAYER_DOWNED_STATE).empty(),
				"request: an enemy Medic receives no 0x54");
	}

	if (failures == 0) std::printf("OK medic_recipient\n");
	return failures == 0 ? 0 : 1;
}
