// opennova-wire --scenario-events: the SCENARIO_EVENT lines the network-parity
// scenario comparator (scripts/net/compare_scenario.py) consumes. The bodies
// are the retail self-nade suicide (S2C 0x13 -> 0x61 -> 0x52 -> 0x1E, the
// joiner at handle 0x005D) and our own encoders' output.

#include "scenario_events.h"

#include <net/npwire/ingame_decode.h>
#include <net/npwire/ingame_encode.h>
#include <net/npwire/ingame_message_id.h>

#include <string>
#include <vector>

#include "common/test_expect.h"

using namespace opennova;

namespace {

InGameMessage message(char dir, uint8_t tag, std::vector<uint8_t> payload, int frame = 7) {
	InGameMessage m;
	m.frame_index = frame;
	m.dir = dir;
	m.tag = tag;
	m.session = 32768;
	m.payload = std::move(payload);
	return m;
}

std::vector<uint8_t> uplink(uint16_t handle, uint16_t carrier) {
	EntityPacketSubHeader header;
	header.handle = handle;
	header.item_type_id = 5305;
	header.sub_op = ENTITY_SUB_OP_EXTENDED;
	PlayerExtendedUplink body;
	body.carrier_handle = carrier;
	std::vector<uint8_t> bytes = encode_entity_packet_sub_header(header);
	const std::vector<uint8_t> tail = encode_player_extended_uplink(body);
	bytes.insert(bytes.end(), tail.begin(), tail.end());
	return bytes;
}

} // namespace

int main() {
	wire::ScenarioEventTracker tracker;
	const auto line = [&](const InGameMessage &m) {
		return wire::format_scenario_event(m, 1791065947917321000ull, tracker);
	};

	// The suicide tail, in retail's emission order.
	TEST_EXPECT(line(message('S', s2c::ENTITY_DEATH, {0x5d, 0x00, 0x00, 0x00})) ==
			"SCENARIO_EVENT frame=7 ts_ns=1791065947917321000 dir=S session=32768 "
			"tag=0x13 kind=entity_death entity=0x005d anim=0 decode=1");
	TEST_EXPECT(line(message('S', s2c::TICK_SEED, {0, 0, 0, 0})).find(
			"tag=0x61 kind=tick_seed seed=0 decode=1") != std::string::npos);
	DeathCameraTarget camera;
	camera.x = -83297049;
	camera.y = 48570439;
	camera.z = 2809448;
	TEST_EXPECT(line(message('S', s2c::DEATH_CAMERA_TARGET, encode_death_camera_target(camera)))
			.find("kind=death_camera pos=-83297049,48570439,2809448 decode=1") !=
			std::string::npos);
	TEST_EXPECT(line(message('S', s2c::GAME_EVENT, {0x02, 0x5d, 0x00, 0x00, 0, 0, 0, 0}))
			.find("tag=0x1e kind=game_event type=2 attacker=93 victim=0 aux=0 pos=0,0 "
			      "decode=1") != std::string::npos);

	// The throw and the reload it triggers.
	ClientFiredRound round;
	round.shooter_handle = 0x005d;
	round.adm_index = 52;
	round.fire_flags = 18;
	TEST_EXPECT(line(message('C', c2s::FIRED_ROUND, encode_client_fired_round(round))).find(
			"dir=C session=32768 tag=0x06 kind=fired_round shooter=0x005d adm=52 flags=18 "
			"target=0xffff decode=1") != std::string::npos);
	WeaponReload reload;
	reload.entity_handle = 0x005d;
	reload.reload_param = 325;
	TEST_EXPECT(line(message('C', c2s::WEAPON_RELOAD_REQUEST, encode_weapon_reload(reload)))
			.find("kind=reload_request entity=0x005d param=325 decode=1") != std::string::npos);
	TEST_EXPECT(line(message('S', s2c::WEAPON_RELOAD, encode_weapon_reload(reload)))
			.find("kind=weapon_reload entity=0x005d param=325 decode=1") != std::string::npos);

	// A short body still prints, flagged.
	TEST_EXPECT(line(message('S', s2c::GAME_EVENT, {0x02})).find("decode=0") !=
			std::string::npos);

	// C2S 0x0C prints the first uplink and each carrier change, nothing else.
	TEST_EXPECT(line(message('C', c2s::ENTITY_UPLINK, uplink(0x005d, 0xffff))).find(
			"kind=carrier handle=0x005d carrier=0xffff decode=1") != std::string::npos);
	TEST_EXPECT(line(message('C', c2s::ENTITY_UPLINK, uplink(0x005d, 0xffff))).empty());
	TEST_EXPECT(line(message('C', c2s::ENTITY_UPLINK, uplink(0x005d, 0x1012))).find(
			"carrier=0x1012") != std::string::npos);
	TEST_EXPECT(line(message('C', c2s::ENTITY_UPLINK, uplink(0x005d, 0xffff))).find(
			"carrier=0xffff") != std::string::npos);

	// Direction is part of the identity: C2S 0x13 is a radio call, S2C 0x06
	// is not a fired round, and a settings-update record is never an event.
	TEST_EXPECT(line(message('C', 0x13, {0x01, 0x00})).empty());
	TEST_EXPECT(line(message('S', 0x06, encode_client_fired_round(round))).empty());
	InGameMessage settings = message('S', s2c::ENTITY_DEATH, {0x5d, 0x00, 0x00, 0x00});
	settings.settings_update = true;
	TEST_EXPECT(line(settings).empty());

	std::printf("opennova-wire scenario events: ok\n");
	return 0;
}
