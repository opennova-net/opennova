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

	explicit Rig(int32_t spectate) {
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
		inmatch::apply_session_create_words(cfg.config, block);
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

} // namespace

int main() {
	test_the_own_slot_and_connection_take_the_word();
	test_the_own_0x75_byte_reads_the_word();
	if (failures != 0) {
		std::printf("%d failure(s)\n", failures);
		return 1;
	}
	std::printf("PASS npruntime_host_spectate\n");
	return 0;
}
