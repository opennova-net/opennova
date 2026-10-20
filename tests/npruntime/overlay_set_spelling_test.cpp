// D-NET-670: the mask-128 S2C 0x34 carries the resolved set's own name, spelled as the first
// loaded bank holding the set spells it: the set is found without case and its name (set+4) is
// copied onto the wire. Pinned through the water-crossing fan with banks that spell BODYWATER1
// and SURFACE_WTR in other case, against literal bytes [flag][cstr][i16 x][i16 y][i16 z].
// [orig: SoundBank_FindTriggerByName @0x75BE90 (stricmp of set+4 @0x75BEBF);
//  NetPacket_WriteOverlayAction @0x505D50 (the type byte @0x505D6E, the copy @0x505D99..0x505DAA,
//  the three words @0x505DC8..0x505DE5); Server_SendOverlayActionToAlive @0x50A1B0]

#include "npruntime/conn_fixture.h"

#include <formats/lwf/lwf.h>
#include <net/npwire/ingame_message_id.h>
#include <runtime/audio/oneshot_play.h>
#include <runtime/inmatch/napi_np_server_ctx.h>
#include <runtime/inmatch/server_entity_routes.h>
#include <runtime/replication/connection_fan.h>
#include <runtime/inmatch/loopback_channel.h>
#include <runtime/world/world.h>

#include <cstdio>
#include <memory>
#include <vector>

using namespace opennova;
namespace w = opennova::world;
namespace ns = opennova::replication;

namespace {

int g_failures = 0;
#define CHECK(c)                                                                     \
	do {                                                                             \
		if (!(c)) {                                                                  \
			std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c);                 \
			++g_failures;                                                            \
		}                                                                            \
	} while (0)

std::vector<uint8_t> body(const char *name, int16_t x, int16_t y, int16_t z) {
	std::vector<uint8_t> out{0x01};
	for (const char *p = name; *p != 0; ++p) out.push_back(static_cast<uint8_t>(*p));
	out.push_back(0x00);
	for (const int16_t v : {x, y, z}) {
		out.push_back(static_cast<uint8_t>(static_cast<uint16_t>(v) & 0xFFu));
		out.push_back(static_cast<uint8_t>(static_cast<uint16_t>(v) >> 8));
	}
	return out;
}

} // namespace

int main() {
	// The catalog is the six global sound-bank slots in slot order: slot 0 spells the airborne set
	// in lower case; slot 1's own spelling of the same name loses (the first loaded slot holding
	// the set wins).
	lwf::File slot0_bank;
	lwf::Multi airborne;
	airborne.name = "bodywater1";
	slot0_bank.multis.push_back(airborne);
	lwf::File slot1_bank;
	lwf::Multi airborne_again;
	airborne_again.name = "BODYWATER1";
	lwf::Multi wade;
	wade.name = "Surface_Wtr";
	slot1_bank.multis.push_back(airborne_again);
	slot1_bank.multis.push_back(wade);
	audio::SoundSetIndex sets;
	sets.add_bank(0, slot0_bank);
	sets.add_bank(1, slot1_bank);
	CHECK(sets.find("BODYWATER1").name == "bodywater1");
	CHECK(sets.find("surface_wtr").name == "Surface_Wtr");

	auto world_owner = std::make_unique<w::World>();
	w::World &world = *world_owner;
	world.tables.sound_sets = &sets;
	world.out.water_crossings.add(3 << 16, 4 << 16, 5 << 16, /*airborne=*/true);
	world.out.water_crossings.add(-2 << 16, 7 << 16, 1 << 16, /*airborne=*/false);
	const std::vector<std::vector<uint8_t>> messages = ns::build_water_cross_messages(world);
	CHECK(messages.size() == 2);
	if (messages.size() == 2) {
		CHECK(messages[0] == body("bodywater1", 3, 4, 5));
		CHECK(messages[1] == body("Surface_Wtr", -2, 7, 1));
	}

	// Through the route: an in-match remote takes the same bytes as S2C 0x34.
	ns::LoopbackChannel remote_wire;
	inmatch::NapiNPServerCtx ctx;
	ctx.is_authority = 1;
	ctx.is_in_session = 1;
	ctx.world = &world;
	ctx.np_protocol.connection_list.push_back(conn_fixture::make_conn(1, 1, &remote_wire,
			ns::TransportMode::Client, w::EntityHandle{}, true));
	inmatch::Server_RouteWaterCrossings(ctx, world);
	std::vector<ns::Datagram> got;
	ns::Datagram d;
	while (remote_wire.client_recv(d)) got.push_back(d);
	CHECK(got.size() == 2);
	if (got.size() == 2) {
		CHECK(got[0].tag == s2c::PLAY_SOUND && got[0].body == body("bodywater1", 3, 4, 5));
		CHECK(got[1].tag == s2c::PLAY_SOUND && got[1].body == body("Surface_Wtr", -2, 7, 1));
	}

	std::printf("overlay_set_spelling: %d failure(s)\n", g_failures);
	return g_failures == 0 ? 0 : 1;
}
