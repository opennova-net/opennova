// The NovaWorld listing's round-init keys: the GameServerBaffleKey (D-NET-984) and the
// cookie-key ring behind PCIDKey (D-NET-986). The round init at the session's create draws
// the key as (GetTickCount() + rand()) % 900000 + 100000, the rand the session CRT stream's next
// draw after the slot table's seed and first draw, the tick the create's, then starts the ring
// from the next draw: slot 0 = ((rand + tick) & 0xFFFFFF) + 0x1000000, the count 1. The server
// tick's refresh then advances the ring from the stream at the round's first tick and every 0x744
// ticks after, running the server-info update each time. The Host list carries the key and the
// ring's current slot. Not the protocol's magic key (+0x530), which no list carries.
// [orig: Server_InitNewRoundState @0x51C8E0 - Server_AllocatePlayerSlotTable (from @0x51C97B),
//  g_ServerInfoUpdateTimer = 0 @0x51CA86, GetTickCount @0x51CB69, rand @0x51CB6D, the add
//  @0x51CB74, div 0xDBBA0 @0x51CB7B, + 0x186A0 @0x51CB82, g_GameServerBaffleKey @0x51CB88,
//  CSessionIdRing_Init (the call @0x51CB8E); CSessionIdRing_Init @0x4DBC20 - rand @0x4DBC45, the
//  store @0x4DBC5B; Server_TickUpdate @0x51D7E0 - @0x51D91D..0x51D943;
//  CSessionIdRing_AdvanceAndGenerate @0x4DBB80; Lobby_UpdateServerInfo @0x4FE8C0 - PCIDKey
//  @0x4FF191..0x4FF196, the key's read @0x4FF1CC; CNapiGameSession_BuildAndCreateSession's call
//  @0x5695B3]

#include <base/io/crt_rand.h>
#include <base/io/tick_rate.h>
#include <net/napi/session.h>
#include <net/novaworld/lobby_vars.h>
#include <net/protocol/protocol.h>
#include <runtime/hud/game_text_lookup.h>
#include <runtime/inmatch/napi_np_server_ctx.h>
#include <runtime/inmatch/server_info.h>
#include <runtime/inmatch/server_session.h>
#include <runtime/inmatch/server_tick_maintenance.h>
#include <runtime/world/world.h>

#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include "../common/test_expect.h"
#include "../common/title_fixture.h"

using namespace opennova;

namespace {

int &failures = test_expect::failures;

using test_expect::expect;

const Protocol &kJo = *title_protocol("jo");

// One dedicated host's create over a seeded session: the key and the ring the round init draws,
// the stream after them, the listing's columns and Host-list vars, then the server tick's refresh.
void check_round_init_draws_the_listing_keys(uint32_t start_tick, uint32_t seed) {
	auto world = std::make_unique<world::World>(); // heap: the 1 MB test stack
	test_titles::jo_world(*world);
	inmatch::NapiNPServerCtx ctx;
	test_titles::jo_config(ctx.config);
	ctx.world = world.get();
	inmatch::set_connection_mode(ctx, inmatch::ConnectionMode::HostOnly);
	inmatch::set_transport_mode(ctx, inmatch::SocketMode::Socketless);
	inmatch::GameConfig settings;
	test_titles::jo_config(settings);
	settings.server_name = "Baffle";
	settings.max_players = 8;
	inmatch::SessionStartup startup;
	startup.protocol = &kJo;
	startup.host_key = 0x12345678u;
	startup.host_start_tick = start_tick;
	startup.session_seed_id = seed;
	inmatch::create_session(ctx, settings, startup, nullptr);

	// The stream: the slot table's seed and first draw, then the key's draw, then the ring's.
	io::CrtRand stream;
	stream.seed(seed);
	(void)stream.next();
	const uint32_t draw = stream.next();
	const uint32_t key = (start_tick + draw) % 0xDBBA0u + 0x186A0u; // 32-bit add, as retail's
	expect(ctx.game_server_baffle_key == key,
	       "the round init draws (tick + rand) % 900000 + 100000");
	expect(ctx.game_server_baffle_key >= 100000u && ctx.game_server_baffle_key <= 999999u,
	       "the key is six digits");
	const uint32_t ring_draw = stream.next();
	const uint32_t slot0 = ((ring_draw + start_tick) & 0xFFFFFFu) + 0x1000000u;
	expect(ctx.cookie_key_table.index == 0 && ctx.cookie_key_table.count == 1 &&
	               ctx.cookie_key_table.keys[0] == slot0,
	       "the ring starts right after the key: slot 0 from the next draw, the count 1");
	expect(ctx.cookie_key_table.keys[1] == 0u && ctx.cookie_key_table.keys[5] == 0u,
	       "the other slots are zero");
	expect(world->crt_rand.state == stream.state,
	       "the init spends two draws of the session stream (the key, the ring)");
	expect(ctx.server_info_update_timer == 0, "the round init clears the refresh's countdown");

	HostRegistration columns;
	inmatch::read_session_listing_columns(columns, ctx, hud::GameTextLookup{});
	expect(columns.game_server_baffle_key == key,
	       "the listing's column reads the round init's key");
	bool listed = false;
	bool pcid_listed = false;
	for (const ClientVar &v : make_host_var_list(columns, HostLobbyText{}, /*full=*/true)) {
		if (v.name == "GameServerBaffleKey") listed = v.value == std::to_string(key);
		if (v.name == "PCIDKey") pcid_listed = v.value == std::to_string(slot0);
	}
	expect(listed, "the Host list carries GameServerBaffleKey");
	expect(columns.pcid_key == slot0 && pcid_listed, "PCIDKey is the ring's current slot");

	// The server tick's refresh: the round's first tick advances the ring (slot 1, the stream's
	// next draw) and runs the update; the next 0x743 ticks do not; the 0x744th does again.
	int updates = 0;
	ctx.on_update_server_info = [&updates]() { ++updates; };
	const uint32_t tick_ms = io::host_milliseconds_for_logic_tick(world->logic_tick);
	const uint32_t first = ((stream.next() + tick_ms) & 0xFFFFFFu) + 0x1000000u;
	inmatch::run_server_info_refresh(ctx, *world);
	expect(updates == 1 && ctx.cookie_key_table.index == 1 && ctx.cookie_key_table.count == 2 &&
	               ctx.cookie_key_table.keys[1] == first && ctx.cookie_key_table.keys[0] == slot0,
	       "the first server tick refreshes: the ring's slot 1 from the stream, the update");
	expect(world->crt_rand.state == stream.state, "the refresh spends one draw");
	expect(ctx.server_info_update_timer == static_cast<int32_t>(SESSION_HOST_INFO_REFRESH_TICKS),
	       "the countdown reloads 0x744");
	for (uint32_t i = 1; i < SESSION_HOST_INFO_REFRESH_TICKS; ++i)
		inmatch::run_server_info_refresh(ctx, *world);
	expect(updates == 1 && ctx.cookie_key_table.index == 1 && world->crt_rand.state == stream.state,
	       "the next 0x743 ticks neither advance nor update");
	(void)stream.next();
	inmatch::run_server_info_refresh(ctx, *world);
	expect(updates == 2 && ctx.cookie_key_table.index == 2 && ctx.cookie_key_table.count == 3 &&
	               world->crt_rand.state == stream.state,
	       "the 0x744th tick refreshes again");
	HostRegistration refreshed;
	inmatch::read_session_listing_columns(refreshed, ctx, hud::GameTextLookup{});
	expect(refreshed.pcid_key == ctx.cookie_key_table.keys[2], "PCIDKey follows the ring");
	ctx.on_update_server_info = nullptr;
}

// A LAN host's refresh advances the ring too (no hook: nothing is published), so its stream
// spends the draw all the same. [orig: Server_TickUpdate @0x51D7E0 - the advance (the call
// @0x51D93D) ahead of Lobby_UpdateServerInfo's gates @0x4FE8D8..0x4FE8FA]
void check_lan_refresh_spends_its_draw() {
	auto world = std::make_unique<world::World>();
	test_titles::jo_world(*world);
	inmatch::NapiNPServerCtx ctx;
	test_titles::jo_config(ctx.config);
	ctx.world = world.get();
	world->crt_rand.seed(77u);
	io::CrtRand stream = world->crt_rand;
	ctx.server_info_update_timer = 0;
	inmatch::run_server_info_refresh(ctx, *world);
	(void)stream.next();
	expect(world->crt_rand.state == stream.state && ctx.cookie_key_table.count == 1 &&
	               ctx.cookie_key_table.index == 1,
	       "an unhooked refresh still advances the ring from the stream");
}

} // namespace

int main() {
	check_round_init_draws_the_listing_keys(100000u, 654321u);
	// A start tick near the wrap: the unsigned 32-bit add wraps before the modulus.
	check_round_init_draws_the_listing_keys(0xFFFFFF00u, 123457u);
	check_lan_refresh_spends_its_draw();
	if (failures == 0) std::printf("npruntime_listing_baffle_key: OK\n");
	return failures == 0 ? 0 : 1;
}
