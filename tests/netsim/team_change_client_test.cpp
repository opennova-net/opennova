// D-NET-282 — the death screen's team change, client side. DEATH shows its
// BUTTON_TEAMLIST / SWAP_TEAMS pair in a session, a non-co-op team game, for
// a local player on a team that is not dead under permanent death, by the
// TeamChoose bit of the rules word (a joiner's S2C 0x08 copy); a SWAP_TEAMS
// click queues one reliable C2S 0x4D with no body and ages every minimap
// overlay timer by 18600 ticks.
// [orig: DeathScreen_UpdateUI @0x553150; DeathScreen_OnSwapTeams @0x5535B0;
//  NetPacket_SendPingRequest @0x42DD90; MapOverlay_UpdateTimers @0x5BFCE0]
#include <runtime/inmatch/client_runtime.h>
#include <runtime/inmatch/role_feeds.h>
#include <runtime/mission/mission_kernel.h>
#include <runtime/replication/client_replica_pipeline.h>
#include <runtime/world/deploy_screen_feed.h>
#include <runtime/world/entity.h>
#include <net/npwire/ingame_message_id.h>
#include <net/npwire/nw_session_framing.h>
#include <net/npwire/session_keys.h>

#include <cstdio>
#include <string>
#include <vector>

using namespace opennova;
using namespace opennova::inmatch;
namespace ns = opennova::replication;

static int failures = 0;
#define CHECK(c)                                                                       \
	do {                                                                               \
		if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } \
	} while (0)

namespace {

// The 51-byte S2C 0x08 body: [10 x i32][7 x u8][u32 rules word].
std::vector<uint8_t> session_config(uint32_t game_type, uint32_t rules) {
	std::vector<uint8_t> body(51, 0);
	for (int i = 0; i < 4; ++i) body[12 + i] = static_cast<uint8_t>(game_type >> (8 * i));
	for (int i = 0; i < 4; ++i) body[47 + i] = static_cast<uint8_t>(rules >> (8 * i));
	return body;
}

// The show rule's truth table [orig: DeathScreen_UpdateUI @0x553412..0x553466].
void test_show_rule() {
	world::DeployTeamButtonsInput in;
	in.in_session = true;
	in.game_type = 0x10000u;
	in.team = 1;
	in.rules_word = 0x4u;
	CHECK(world::deploy_team_buttons_shown(in));
	world::DeployTeamButtonsInput off = in;
	off.rules_word = 0x3A02u; // the cfg default: TeamChoose clear
	CHECK(!world::deploy_team_buttons_shown(off));
	off = in;
	off.in_session = false;
	CHECK(!world::deploy_team_buttons_shown(off));
	off = in;
	off.game_type = 0x30000u; // co-op
	CHECK(!world::deploy_team_buttons_shown(off));
	off = in;
	off.game_type = 0x1u; // no team bit
	CHECK(!world::deploy_team_buttons_shown(off));
	off = in;
	off.team = 0;
	CHECK(!world::deploy_team_buttons_shown(off));
	off = in;
	off.permanent_death = true;
	CHECK(world::deploy_team_buttons_shown(off)); // alive under permanent death
	off.dead = true;
	CHECK(!world::deploy_team_buttons_shown(off));
	off = in;
	off.dead = true; // dead without permanent death still shows
	CHECK(world::deploy_team_buttons_shown(off));
}

// A joiner reads the rules word its S2C 0x08 carried.
void test_joiner_feed_reads_the_session_rules() {
	ClientRuntime runtime("TeamButtons");
	mission::MissionKernel kernel;
	kernel.world.rules.mp_session = true;
	kernel.world.registry.configure_pool(0, 8);
	world::Entity seed;
	seed.kind = world::EntityKind::Organic;
	seed.team = 1;
	kernel.world.cached.local_player = kernel.world.registry.spawn(0, seed);
	RoleView joiner;
	joiner.kernel = &kernel;
	joiner.runtime = &runtime;
	joiner.joiner = true;
	joiner.staged_mp_attributes = 0x4u; // the joiner's own word is not read
	runtime.view().apply(s2c::SESSION_CONFIG, session_config(0x10000u, 0x0u));
	CHECK(runtime.state().session_rules_flags == 0u);
	CHECK(!deploy_screen_status(joiner, world::SpawnZoneRegistry(), "M",
			hud::GameTextLookup()).team_buttons_shown);
	runtime.view().apply(s2c::SESSION_CONFIG, session_config(0x10000u, 0x4u));
	CHECK(runtime.state().session_rules_flags == 0x4u);
	CHECK(deploy_screen_status(joiner, world::SpawnZoneRegistry(), "M",
			hud::GameTextLookup()).team_buttons_shown);
}

// The click: one empty reliable C2S 0x4D at the next send boundary, and the
// minimap's transient overlays aged out by the 18600-tick step.
void test_joiner_sends_the_request() {
	const std::string client_scrk = "CLIENT-TEAM-REQ-SCRK";
	const std::string server_scrk = "SERVER-TEAM-REQ-SCRK";
	ClientRuntime client("TeamRequest");
	CHECK(!client.queue_team_change_request()); // no session yet
	client.seed_session(0x4D4D0001u, 1u, client_scrk, server_scrk,
			1, 0, 0x0002, world::kPlayerInfantryTypeId);
	// One transient overlay slot (a 0x40 zone record) in the client's banks.
	auto &e = client.view().state().upsert(0x2031);
	e.cls = EntityClass::Vehicle;
	client.view().apply(s2c::CAPTURE_ZONE_STATE, {1, 0x31, 0x20, 10, 0x0A, 0, 0});
	bool held = false;
	for (const auto &slot : client.view().state().minimap.transient)
		held = held || (slot.active && slot.handle == 0x2031);
	CHECK(held);
	CHECK(client.queue_team_change_request());
	held = false;
	for (const auto &slot : client.view().state().minimap.transient)
		held = held || (slot.active && slot.handle == 0x2031);
	CHECK(!held);
	std::vector<std::vector<uint8_t>> sent;
	for (const std::vector<uint8_t> &datagram : client.Client_ProcessNetworkFrame(1)) {
		uint8_t opcode = 0;
		std::vector<uint8_t> plain;
		ProtocolPacketHeader header;
		std::vector<ProtocolMessage> messages;
		if (!nw_decode_inbound(datagram.data(), datagram.size(), opcode, plain) ||
				opcode != SESSION_OPCODE_PROTOCOL_MESSAGE ||
				!decode_protocol_packet_plaintext(plain.data(), plain.size(), client_scrk,
						header, messages))
			continue;
		for (const ProtocolMessage &m : messages)
			if (m.tag == c2s::TEAM_CHANGE_REQUEST) {
				sent.push_back(m.payload);
				CHECK(m.reliable);
			}
	}
	CHECK(sent.size() == 1);
	if (sent.size() == 1) CHECK(sent[0].empty());
}

} // namespace

int main() {
	test_show_rule();
	test_joiner_feed_reads_the_session_rules();
	test_joiner_sends_the_request();
	std::printf("team_change_client: %d failures\n", failures);
	return failures ? 1 : 0;
}
