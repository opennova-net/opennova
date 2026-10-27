// D-NET-1368 (L4-s39 row 3): the listen host's own client runs every relayed S2C 0x49 the
// way any client does. The request handler relays each reload to every in-match connection, the
// host's own included (masks 0xC0 and 0x20, no NOT_HOST bit), and the client handler dips the arms
// of a person it names, other than the local player, for 80 ticks. So on a listen host's screen a
// remote player whose reload rides the relay dips its arms, as on a joiner's. The local player's
// arm and a non-person's arm refill, which the authority already did, so they add nothing, and a
// row with no def takes neither arm.
// [orig: NapiNPServerMsg_HandleReloadRequest @0x514DF0 - the relays called @0x514EBD /
//  @0x514EE3; NapiNPServer_SendFiltered @0x4C87E0 - the 0x10 test @0x4C88F1..0x4C88FA;
//  NapiNPClientMsg_WeaponReload_0x049 @0x42C0A0 - the local test @0x42C0F2..0x42C0F8, the
//  itemDef test @0x42C0FE..0x42C103, `cmp [ecx+5Ch], 3` @0x42C105, the stamp @0x42C10B]

#include <net/npwire/ingame_decode.h>
#include <net/npwire/ingame_encode.h>
#include <net/npwire/ingame_message_id.h>
#include <runtime/inmatch/client_runtime.h>
#include <runtime/inmatch/loopback_channel.h>
#include <runtime/world/ai.h>
#include <runtime/world/world.h>

#include <cstdint>
#include <cstdio>
#include <memory>

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

w::EntityHandle spawn_body(w::World &world, int pool, bool has_def, uint8_t item_type) {
	w::Entity seed;
	seed.alive = true;
	seed.health = 100;
	seed.has_item_def = has_def;
	seed.item_type = item_type;
	const w::EntityHandle h = world.registry.spawn(pool, seed);
	world.ai.attach(h);
	return h;
}

int32_t dip(w::World &world, w::EntityHandle h) {
	const w::AiEntity *body = world.ai.for_handle(h);
	return body != nullptr ? body->inf.arms_dip_ticks : -1;
}

void echo(replication::LoopbackChannel &loop, w::EntityHandle h) {
	WeaponReload reload;
	reload.entity_handle = h.packed;
	reload.reload_param = 0x0102;
	loop.host_send(s2c::WEAPON_RELOAD, encode_weapon_reload(reload));
}

void test_the_own_client_dips_a_named_persons_arms() {
	auto world = std::make_unique<w::World>();
	world->registry.configure_pool(0, 8);
	world->registry.configure_pool(1, 4);
	const w::EntityHandle local = spawn_body(*world, 0, true, 3);
	world->cached.local_player = local;
	const w::EntityHandle remote = spawn_body(*world, 0, true, 3);
	const w::EntityHandle defless = spawn_body(*world, 0, false, 0);
	const w::EntityHandle gun = spawn_body(*world, 1, true, 2);
	replication::LoopbackChannel loop;
	auto own = std::make_unique<inmatch::ClientRuntime>(loop);
	for (const w::EntityHandle h : {local, remote, defless, gun}) echo(loop, h);
	(void)own->Client_ProcessNetworkFrame(0);
	// The echoes wait for the fold between two server ticks.
	CHECK(dip(*world, remote) == 0);
	own->apply_received_effects(*world);
	CHECK(dip(*world, remote) == 80);
	CHECK(dip(*world, local) == 0);
	CHECK(dip(*world, defless) == 0);
	CHECK(dip(*world, gun) == 0);
	// Each echo applies once: the next fold restamps nothing.
	world->ai.for_handle(remote)->inf.arms_dip_ticks = 12;
	(void)own->Client_ProcessNetworkFrame(0);
	own->apply_received_effects(*world);
	CHECK(dip(*world, remote) == 12);
}

} // namespace

int main() {
	test_the_own_client_dips_a_named_persons_arms();
	if (failures == 0) std::printf("host_reload_echo_test: all passed\n");
	return failures == 0 ? 0 : 1;
}
