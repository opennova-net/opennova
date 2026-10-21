// The NovaWorld listing's GameServerBaffleKey (D-NET-NEW-L9-11): the round init at the session's
// create draws it as (GetTickCount() + rand()) % 900000 + 100000, the rand the session CRT stream's
// next draw after the slot table's seed and first draw, the tick the create's; the Host list
// carries it. Not the protocol's magic key (+0x530), which no list carries.
// [orig: Server_InitNewRoundState @0x51C8E0 - Server_AllocatePlayerSlotTable (from @0x51C97B),
//  GetTickCount @0x51CB69, rand @0x51CB6D, the add @0x51CB74, div 0xDBBA0 @0x51CB7B, + 0x186A0
//  @0x51CB82, g_GameServerBaffleKey @0x51CB88; Lobby_UpdateServerInfo @0x4FE8C0 - the read @0x4FF1CC;
//  CNapiGameSession_BuildAndCreateSession's call @0x5695B3]

#include <base/gameprofile/gameprofile.h>
#include <base/io/crt_rand.h>
#include <net/novaworld/lobby_vars.h>
#include <net/title/title_identity.h>
#include <runtime/hud/game_text_lookup.h>
#include <runtime/inmatch/napi_np_server_ctx.h>
#include <runtime/inmatch/server_info.h>
#include <runtime/inmatch/server_session.h>
#include <runtime/world/world.h>

#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>

using namespace opennova;

namespace {

int failures = 0;

bool expect(bool cond, const char *what) {
	if (!cond) {
		std::printf("FAIL: %s\n", what);
		++failures;
	}
	return cond;
}

const TitleIdentity &kJo = *title_identity(gameprofile::GAME_JO);

// One dedicated host's create over a seeded session: the key the round init draws, the stream
// after it, and the listing's column and Host-list var.
void check_round_init_draws_the_listing_key(uint32_t start_tick, uint32_t seed) {
	auto world = std::make_unique<world::World>(); // heap: the 1 MB test stack
	inmatch::NapiNPServerCtx ctx;
	ctx.world = world.get();
	inmatch::set_connection_mode(ctx, inmatch::ConnectionMode::HostOnly);
	inmatch::set_transport_mode(ctx, inmatch::SocketMode::Socketless);
	inmatch::GameConfig settings;
	settings.server_name = "Baffle";
	settings.max_players = 8;
	inmatch::SessionStartup startup;
	startup.title = &kJo;
	startup.host_key = 0x12345678u;
	startup.host_start_tick = start_tick;
	startup.session_seed_id = seed;
	inmatch::create_session(ctx, settings, startup, nullptr);

	// The stream: the slot table's seed and first draw, then the key's draw.
	io::CrtRand stream;
	stream.seed(seed);
	(void)stream.next();
	const uint32_t draw = stream.next();
	const uint32_t key = (start_tick + draw) % 0xDBBA0u + 0x186A0u; // 32-bit add, as retail's
	expect(ctx.game_server_baffle_key == key,
	       "the round init draws (tick + rand) % 900000 + 100000");
	expect(ctx.game_server_baffle_key >= 100000u && ctx.game_server_baffle_key <= 999999u,
	       "the key is six digits");
	expect(world->crt_rand.state == stream.state, "the init spends one draw of the session stream");

	HostRegistration columns;
	inmatch::read_session_listing_columns(columns, ctx, hud::GameTextLookup{});
	expect(columns.game_server_baffle_key == key,
	       "the listing's column reads the round init's key");
	bool listed = false;
	for (const ClientVar &v : make_host_var_list(columns, HostLobbyText{}, /*full=*/true)) {
		if (v.name == "GameServerBaffleKey") listed = v.value == std::to_string(key);
	}
	expect(listed, "the Host list carries GameServerBaffleKey");
}

} // namespace

int main() {
	check_round_init_draws_the_listing_key(100000u, 654321u);
	// A start tick near the wrap: the unsigned 32-bit add wraps before the modulus.
	check_round_init_draws_the_listing_key(0xFFFFFF00u, 123457u);
	if (failures == 0) std::printf("npruntime_listing_baffle_key: OK\n");
	return failures == 0 ? 0 : 1;
}
