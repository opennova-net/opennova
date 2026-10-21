// D-NET-NEW-L4-10: the host's own pre.mnu SPECTATE word (game.cfg +0x428). On a session peer
// (a listen host, the single player) the create's round-state init latches the own slot's
// spectator byte from its low byte, the create puts the word in the own connection's JSR, and
// the own slot's S2C 0x75 flag byte reads the word itself, normalized to 1; the own add takes
// no spectator leg (its arm jumps past the remote arm's).
// [orig: Server_InitNewRoundState - is_mp_session_peer @0x51CA4D, the byte
//  @0x51CA67..0x51CA6D; CNapiGameSession_BuildAndCreateSession @0x5697FD..0x569817;
//  NetPacket_SerializeHostEntityState - the own slot's word @0x5108A1..0x5108A7, the
//  normalization @0x5108B9..0x5108BF; Server_PlayerAdd - the own arm @0x51CC24..0x51CC60, the
//  remote arm's leg @0x51D0B7..0x51D0ED]
// The world, the mission and the config are the test's own.

#include <runtime/inmatch/host_config.h>
#include <runtime/inmatch/host_session.h>
#include <runtime/inmatch/loopback_channel.h>
#include <runtime/inmatch/napi_np_connection.h>
#include <runtime/inmatch/napi_np_server_ctx.h>
#include <runtime/inmatch/server_message_dispatch.h>
#include <runtime/inmatch/server_spawn.h>
#include <base/gameprofile/gameprofile.h>
#include <formats/gamecfg/game_cfg.h>
#include <formats/mission/bms.h>
#include <net/npwire/ingame_message_id.h>
#include <net/npwire/protocol_message.h>
#include <net/title/title_identity.h>
#include <runtime/world/world.h>

#include <cstdint>
#include <cstdio>
#include <memory>
#include <vector>

namespace {

using namespace opennova;
namespace inmatch = opennova::inmatch;
namespace w = opennova::world;

int failures = 0;
#define CHECK(c)                                                                               \
	do {                                                                                       \
		if (!(c)) {                                                                            \
			std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c);                           \
			++failures;                                                                        \
		}                                                                                      \
	} while (0)

// A playing LAN host whose cfg block carries `spectate` at +0x428, brought up through the
// shell's own path (the cfg block's session-create words, then the session start).
struct Rig {
	std::unique_ptr<w::World> world = std::make_unique<w::World>();
	std::unique_ptr<bms::File> mission = std::make_unique<bms::File>();
	std::unique_ptr<inmatch::HostOwner> owner = std::make_unique<inmatch::HostOwner>();
	replication::LoopbackChannel loop;

	explicit Rig(int32_t spectate, int32_t side_req = -1, const char *side_a_password = "",
			const char *side_b_password = "") {
		owner->host_loopback = &loop;
		owner->ctx.world = world.get();
		owner->ctx.mission = mission.get();
		world->registry.configure_pool(0, 4);
		inmatch::HostConfig cfg;
		cfg.title = opennova::title_identity(gameprofile::GAME_JO);
		cfg.config.server_name = "spectate";
		cfg.config.max_players = 4;
		cfg.config.game_type = 0x10000u;
		cfg.socket_mode = inmatch::SocketMode::Lan;
		cfg.serve_and_play = true;
		gamecfg::GameCfg block;
		block.join_spectate = spectate;
		block.side_req = side_req;
		inmatch::apply_session_create_words(cfg.config, block);
		cfg.config.side_a_password = side_a_password;
		cfg.config.side_b_password = side_b_password;
		inmatch::start_host_session(*owner, cfg);
	}
	inmatch::NapiNPConnection *own() {
		for (inmatch::NapiNPConnection &c : owner->ctx.np_protocol.connection_list)
			if (c.type == inmatch::NapiNPConnection::kTypeClientSide) return &c;
		return nullptr;
	}
	// The own client's C2S 0x47 through the shared dispatcher: the S2C 0x75's first byte.
	int tag75_byte0() {
		inmatch::NapiNPConnection *c = own();
		if (c == nullptr) return -1;
		return tag75_byte0(*c);
	}
	// A connection's C2S 0x47 through the shared dispatcher: the S2C 0x75's first byte.
	int tag75_byte0(inmatch::NapiNPConnection &conn) {
		inmatch::NapiNPConnection *c = &conn;
		std::vector<ProtocolMessage> in;
		in.push_back(make_protocol_message(c2s::PING, {}));
		inmatch::ServerDispatchInputs inputs;
		inputs.server_ctx = &owner->ctx;
		const std::vector<ProtocolMessage> out = inmatch::dispatch_session_replies(
				owner->ctx.config, *c, in, 0, owner->ctx.np_protocol.connection_list,
				world.get(), inputs);
		for (const ProtocolMessage &m : out)
			if (m.tag == s2c::SPECTATOR_FLAGS && !m.payload.empty()) return m.payload[0];
		return -1;
	}
};

// The own slot's latch and the own connection's JSR follow the word on a session peer; the
// own add's body takes no spectator leg.
void test_the_own_slot_and_connection_take_the_word() {
	for (const int32_t spectate : {0, 1}) {
		Rig r(spectate);
		CHECK(r.owner->ctx.config.join_spectate == spectate);
		inmatch::NapiNPConnection *own = r.own();
		CHECK(own != nullptr);
		if (own == nullptr) continue;
		CHECK(own->link.spectator == (spectate != 0));
		CHECK(own->join_spectator_request == static_cast<uint8_t>(spectate));
		const w::Entity *body = r.world->registry.get(own->link.owned_entity);
		CHECK(body != nullptr);
		if (body != nullptr) CHECK(body->damage_state != -1);
	}
}

// The own slot's 0x75 reads the word itself, not the slot's latch.
void test_the_own_0x75_byte_reads_the_word() {
	{
		Rig r(1);
		CHECK(r.tag75_byte0() == 1);
		if (inmatch::NapiNPConnection *own = r.own()) own->link.spectator = false;
		CHECK(r.tag75_byte0() == 1);
	}
	{
		Rig r(0);
		CHECK(r.tag75_byte0() == 0);
		if (inmatch::NapiNPConnection *own = r.own()) own->link.spectator = true;
		CHECK(r.tag75_byte0() == 0);
	}
}

// D-NET-NEW-L4-10 FOUND 3: a remote slot's byte reads its connection's latch, set at its join
// from a nonzero JSR; a C2S 0x51 convert latches the slot alone, so a player converted in the
// match still reads 0, and a joined spectator reads 1 whatever its slot latch.
// [orig: NetPacket_SerializeHostEntityState - the connection entry's +0x37 `setnz`
//  @0x5108AF..0x5108B6; NapiNPServer_HandlePlayerJoinMessage @0x512E57..0x512E60;
//  Server_ConvertPlayerToSpectator - the slot latch alone @0x519E61]
void test_a_remote_0x75_byte_reads_the_connection_latch() {
	Rig r(0);
	inmatch::NapiNPConnection remote;
	remote.type = 1;
	remote.connection_id = 3;
	remote.admission_stage = inmatch::GameAdmissionStage::Complete;
	remote.assigned_team = 2;
	remote.assigned_team_valid = true;
	// A player converted by C2S 0x51: the slot latch set, the connection's not.
	remote.link.spectator = true;
	remote.join_spectator_request = 0;
	CHECK(r.tag75_byte0(remote) == 0);
	// A spectator by its join's JSR.
	remote.join_spectator_request = 1;
	CHECK(r.tag75_byte0(remote) == 1);
	remote.link.spectator = false;
	CHECK(r.tag75_byte0(remote) == 1);
}

// D-NET-NEW-L4-10 FOUND 5: a map change's round init has no spectator leg. Its re-add of a
// kept spectator slot (a player the match converted: the slot latch, team 0) spawns the body
// with the slot's team word and the 620-tick protection, which the slot's first 0x0A then
// restamps (-1, Health 1, hidden); the remote add's -1 is the pump's alone.
// [orig: Server_InitAllPlayerEntitiesForRound @0x516AA0 - the body's team @0x516B78..0x516B7E,
//  620 @0x516BBA; NetPacket_WritePlayerState @0x4FF6FA..0x4FF728; Server_PlayerAdd's remote
//  arm's leg @0x51D0B7..0x51D0ED]
void test_a_map_changes_re_add_takes_no_spectator_leg() {
	Rig r(0);
	inmatch::NapiNPConnection joiner;
	joiner.type = 1;
	joiner.connection_id = inmatch::kFirstJoinerDcb;
	joiner.self_id_seen = true;
	joiner.phase = inmatch::ConnectionPhase::Joined;
	r.owner->ctx.np_protocol.connection_list.push_back(joiner);
	CHECK(inmatch::Server_ProcessPendingPlayerSpawns(r.owner->ctx, *r.world) == 1);
	inmatch::NapiNPConnection &c = r.owner->ctx.np_protocol.connection_list.back();
	c.link.spectator = true;
	c.assigned_team = 0;
	c.assigned_team_valid = true;
	const w::EntityHandle h =
			inmatch::Server_BuildPlayerInfoAndAdd(r.owner->ctx, c, *r.world, /*round_init=*/true);
	const w::Entity *body = r.world->registry.get(h);
	CHECK(body != nullptr);
	if (body == nullptr) return;
	CHECK(body->damage_state == 620 && body->team == 0);
	CHECK((body->flags & w::kEntityFlagCarried) != 0);
}

// The create's profile init overwrites the own slot's team after the own add: the net config's
// side byte when it is 1 or 2, else 1 for a slot whose spectator latch is clear and 0 for one
// that is set; the add's PlayerList entry keeps the round init's assignment (a team game's
// balance: 1; a spectator's 0), and the body takes the overwritten team. The side byte is -1 on
// every host (its cfg key is dead and the pre.mnu commit stores -1), so the reachable case is a
// locked side: side A's password alone sends the round init's assignment to side B's team 2
// and both to 0, where the profile init leaves 1.
// [orig: PlayerSession_InitFromProfile @0x50CA92..0x50CAB8, the store @0x50CB72;
//  CNapiGameSession_CreateSession @0x4C9C9A..0x4C9CCE; Server_PlayerAdd's own arm, the
//  bodyless entry @0x51D398]
void test_the_create_overwrites_the_own_slots_team() {
	struct Case {
		int32_t spectate;
		int32_t side_req;
		uint8_t team;
		int32_t list_team;
	};
	for (const Case c : {Case{0, -1, 1, 1}, Case{1, -1, 0, 0}, Case{0, 2, 2, 1}, Case{1, 2, 2, 0},
				Case{0, 1, 1, 1}, Case{1, 0, 0, 0}, Case{0, 0, 1, 1}}) {
		Rig r(c.spectate, c.side_req);
		inmatch::NapiNPConnection *own = r.own();
		CHECK(own != nullptr);
		if (own == nullptr) continue;
		CHECK(own->assigned_team_valid && own->assigned_team == c.team);
		CHECK(own->player_list_team == c.list_team);
		const w::Entity *body = r.world->registry.get(own->link.owned_entity);
		CHECK(body != nullptr && body->team == c.team);
	}
	struct Locked {
		const char *side_a;
		const char *side_b;
		int32_t list_team;
	};
	for (const Locked c : {Locked{"alpha", "", 2}, Locked{"", "bravo", 1}, Locked{"alpha", "bravo", 0}}) {
		Rig r(0, -1, c.side_a, c.side_b);
		inmatch::NapiNPConnection *own = r.own();
		CHECK(own != nullptr);
		if (own == nullptr) continue;
		CHECK(own->assigned_team == 1 && own->player_list_team == c.list_team);
		const w::Entity *body = r.world->registry.get(own->link.owned_entity);
		CHECK(body != nullptr && body->team == 1);
	}
}

} // namespace

int main() {
	test_the_own_slot_and_connection_take_the_word();
	test_the_own_0x75_byte_reads_the_word();
	test_the_create_overwrites_the_own_slots_team();
	test_a_map_changes_re_add_takes_no_spectator_leg();
	test_a_remote_0x75_byte_reads_the_connection_latch();
	if (failures != 0) {
		std::printf("%d failure(s)\n", failures);
		return 1;
	}
	std::printf("PASS npruntime_host_spectate\n");
	return 0;
}
