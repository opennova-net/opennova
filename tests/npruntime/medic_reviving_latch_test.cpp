// The joiner's being-revived latch (the local entity's +0x1E0 word, ClientState::
// local_medic_reviving): the S2C 0x3A handler sets it, and the local respawn's
// Game_InitNewRound clears it. Retail runs that Game_InitNewRound inside the 0x0A record
// decode, in the self record's respawn arm. So in one pump a 0x3A after the reviving 0x0A
// leaves the latch set, and a 0x3A ahead of it is cleared. Nothing after the receive clears
// it again: not the replica walk's visit of the self row, and not L's revive on the next
// tail.
// [orig: NetPacket_SerializePlayerState @0x4C09C0 - `test bl, 2` @0x4C0FE1, `test byte
//  [edi+24h], 2` @0x4C1109, Game_InitNewRound called @0x4C114C; Game_InitNewRound @0x422740 -
//  `mov [eax+1E0h], ebx` @0x4227D4; NapiNPClientMsg_0x03A @0x422680]

#include <net/npwire/idatagram_socket.h>
#include <net/npwire/ingame_decode.h>
#include <net/npwire/ingame_encode.h>
#include <net/npwire/ingame_message_id.h>
#include <net/npwire/nw_session_framing.h>
#include <net/npwire/protocol_message.h>
#include <net/npwire/session_keys.h>
#include <net/protocol/protocol.h>
#include <base/gameprofile/gameprofile.h>
#include <runtime/inmatch/client_runtime.h>
#include <runtime/inmatch/joiner_role.h>
#include <runtime/inmatch/napi_np_connection.h> // make_jo_game_session_sequencing
#include <runtime/mission/mission_kernel.h>
#include <runtime/world/entity.h>
#include <runtime/world/player_spawn.h>

#include <cstdio>
#include <memory>
#include <string>
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

const opennova::Protocol &kJo = *opennova::title_protocol("jo");
const std::string kClientScrk = "CLIENT-BRIDGE-SCRK";
const std::string kServerScrk = "SERVER-BRIDGE-SCRK";
constexpr uint32_t kClientKey = 0x0000BEEFu;
constexpr uint32_t kSessionId = 0x0FE0E112u;
constexpr uint16_t kSelf = 0x0005;

class NullSocket final : public opennova::IDatagramSocket {
public:
	int recv_from(uint8_t *, std::size_t, PeerAddr &) override { return 0; }
	void send_to(const PeerAddr &, const uint8_t *, std::size_t) override {}
};

// joiner_role_test's harness, driving the revive deploy of
// run_revive_deploy_respawns_l_on_its_record_edge (D-NET-379).
struct Harness {
	std::unique_ptr<mission::MissionKernel> kernel = std::make_unique<mission::MissionKernel>();
	inmatch::JoinerRole role;
	NullSocket socket;
	inmatch::TickInput input;
	SessionSequencing seq = inmatch::make_jo_game_session_sequencing();
	Harness() {
		kernel->world.registry.configure_pool(0, 16);
		role.bind(*kernel);
		role.set_socket(&socket, PeerAddr{});
		role.set_protocol(&kJo);
		role.create_runtime("BridgeJoiner", inmatch::JoinRole::Player, "", "");
		role.kit_seams.apply_authoritative = [] {};
		role.kit_seams.reseed_on_side_change = [] { return false; };
		role.kit_seams.push = [] {};
		role.poll_preload();
		role.seed_session(kSessionId, kClientKey, kClientScrk, kServerScrk, 1, 0,
				kSelf, w::kPlayerInfantryTypeId);
		role.run_tick(input);
		auto &self_row = role.runtime->state().upsert(kSelf);
		self_row.cls = EntityClass::Player;
		self_row.type_id = w::kPlayerInfantryTypeId;
	}
	static ProtocolMessage self_record(int16_t health, uint8_t state_flags) {
		FrameUpdate fu;
		fu.carried_handle = 0xFFFF;
		fu.health = health;
		FrameUpdateRecord rec;
		rec.handle = kSelf;
		rec.type_id = w::kPlayerInfantryTypeId;
		rec.cls = EntityClass::Player;
		rec.player.carrier_handle = 0xFFFF;
		rec.player.state_flags = state_flags;
		fu.records.push_back(rec);
		return make_protocol_message(s2c::PER_FRAME_UPDATE, encode_frame_update(fu));
	}
	static ProtocolMessage reviving() { return make_protocol_message(s2c::MEDIC_REVIVING, {}); }
	// One receive holding `messages` in order, then the joiner's frame.
	void pump(std::vector<ProtocolMessage> messages) {
		std::vector<uint8_t> packet;
		frame_session_packet(seq, SessionCrypto{kServerScrk, {}, kClientKey}, messages, packet);
		auto datagram = nw_encode_outbound(SESSION_OPCODE_SERVER_PROTOCOL_MESSAGE, std::move(packet));
		role.runtime->receive(datagram.data(), datagram.size());
		role.run_tick(input);
	}
	bool latched() const { return role.runtime->state().local_medic_reviving; }
	// L dies on a dead self record and the player picks Default Spawn.
	void die_and_pick() {
		pump({self_record(0, static_cast<uint8_t>(w::kEntityFlagDead))});
		CHECK(role.runtime->deployment_pick_pending());
		CHECK(role.runtime->queue_deployment_pick(0xFFFF));
		role.run_tick(input);
	}
};

// A 0x3A after the reviving record in one pump: the record's Game_InitNewRound has already
// cleared the latch, so the 0x3A leaves it set, past the frame's visit of the self row and
// past L's revive on the next tail.
void test_a_0x3a_after_the_reviving_record_stays_latched() {
	Harness h;
	h.pump({Harness::self_record(100, 0x00)});
	h.die_and_pick();
	h.pump({Harness::self_record(100, 0x00), Harness::reviving()});
	CHECK(h.latched());
	CHECK(!h.role.runtime->deployment_pick_pending()); // the arm released the deploy
	h.pump({Harness::self_record(100, 0x00)}); // the next tail revives L
	const w::Entity *local = h.kernel->local.player();
	CHECK(local != nullptr && local->alive);
	CHECK(h.latched());
}

// A 0x3A ahead of the reviving record in one pump: set, then cleared by the record's
// Game_InitNewRound.
void test_a_0x3a_ahead_of_the_reviving_record_is_cleared() {
	Harness h;
	h.pump({Harness::self_record(100, 0x00)});
	h.die_and_pick();
	h.pump({Harness::reviving()});
	CHECK(h.latched()); // a medic reviving the dead player
	h.pump({Harness::reviving(), Harness::self_record(100, 0x00)});
	CHECK(!h.latched());
	h.pump({Harness::self_record(100, 0x00)});
	CHECK(!h.latched());
}

// A dead record with no respawn (L still dead) never clears the latch: the arm needs an
// alive record over a dead local entity.
void test_a_dead_record_keeps_the_latch() {
	Harness h;
	h.pump({Harness::self_record(100, 0x00)});
	h.die_and_pick();
	h.pump({Harness::reviving()});
	h.pump({Harness::self_record(0, static_cast<uint8_t>(w::kEntityFlagDead))});
	CHECK(h.latched());
}

} // namespace

int main() {
	test_a_0x3a_after_the_reviving_record_stays_latched();
	test_a_0x3a_ahead_of_the_reviving_record_is_cleared();
	test_a_dead_record_keeps_the_latch();
	if (failures != 0) {
		std::printf("%d check(s) failed\n", failures);
		return 1;
	}
	std::printf("medic_reviving_latch: ok\n");
	return 0;
}
