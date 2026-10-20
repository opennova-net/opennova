// The single player's join places nothing (D-NET-NEW-L1-19): retail's join re-places
// the body through the no-pick arm only in a session, and the single player runs with
// is_in_session 0, so its own body keeps the mission start's placement; in a session
// the join's placement moves the body to the start marker again.
// [orig: Server_OnPlayerJoin `cmp g_NapiNPCtx.is_in_session, ebx` @0x51A777, `jz`
//  @0x51A77D, the Server_PositionPlayerForSpawn call @0x51A786]
// The host's own add files no /PROFILE row (the local arm jumps past it); the mission
// start's roster pass files it. [orig: Server_PlayerAdd - the local arm @0x51CC29 ->
//  `jmp loc_51D314` @0x51CC60, past the write @0x51D2FD..0x51D30F]
#include <runtime/inmatch/loopback_channel.h>
#include <runtime/inmatch/napi_np_connection.h>
#include <runtime/inmatch/napi_np_server_ctx.h>
#include <runtime/inmatch/server_files.h>
#include <runtime/inmatch/server_initial_state.h>
#include <runtime/inmatch/server_log_recorder.h>
#include <runtime/inmatch/server_session.h>
#include <runtime/inmatch/server_spawn.h>
#include <runtime/world/entity.h>
#include <runtime/world/world.h>
#include <net/npwire/serverlog_decode.h>

#include <cstdio>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "host_test_setup.h"

using namespace opennova;
namespace w = opennova::world;
namespace ns = opennova::replication;

namespace {

int failures = 0;
bool expect(bool cond, const char *msg) {
	if (cond) return true;
	std::fprintf(stderr, "FAIL: %s\n", msg);
	++failures;
	return false;
}

// A Deathmatch host's own player on a mission with one DM start marker: the add
// places the body on the marker; the body then stands elsewhere (its motor moved
// it) when its join runs. Returns the body's x word after the join.
int32_t join_x_after_moving(bool mp_session) {
	auto world = std::make_unique<w::World>();
	world->registry.configure_pool(0, 16);
	world->registry.configure_pool(3, 16);
	world->rules.mp_session = mp_session;
	w::Entity start;
	start.kind = w::EntityKind::Marker;
	start.item_id = 6095; // the DM start marker
	start.position = {100.0f, 0.0f, 0.0f};
	world->registry.spawn(3, start);
	ns::LoopbackChannel loop;
	inmatch::NapiNPServerCtx ctx;
	inmatch::GameConfig settings;
	settings.server_name = "DM";
	settings.max_players = 2;
	settings.game_type = 0;
	inmatch::test::bring_up_host(ctx, inmatch::ConnectionMode::HostClient,
			inmatch::SocketMode::Socketless, /*host_key=*/0, &loop, settings);
	ctx.world = world.get();
	inmatch::Server_InitNewRoundState(ctx);
	expect(inmatch::Server_ProcessPendingPlayerSpawns(ctx, *world) == 1, "the own add runs");
	w::Entity *body = world->registry.get(world->cached.local_player);
	if (!expect(body != nullptr, "the own add binds the local player")) return 0;
	expect(body->position.x.q16 == 100 * 65536, "the add places the body on the marker");
	body->position.x.q16 = 5 * 65536;
	inmatch::NapiNPConnection &own = ctx.np_protocol.connection_list.front();
	(void)inmatch::Server_OnPlayerJoin(ctx, own, 1, {0, 0});
	return world->registry.get(world->cached.local_player)->position.x.q16;
}

class MemoryFiles final : public inmatch::ServerFileSink {
public:
	std::map<std::string, std::string> files;
	bool exists(const std::string &name) override { return files.count(name) != 0; }
	void remove(const std::string &name) override { files.erase(name); }
	bool write(const std::string &name, std::string_view bytes, bool append) override {
		std::string &f = files[name];
		if (!append) f.clear();
		f.append(bytes.data(), bytes.size());
		return true;
	}
};

void test_the_own_add_files_no_profile_row() {
	auto world = std::make_unique<w::World>();
	world->registry.configure_pool(0, 16);
	world->registry.configure_pool(3, 16);
	world->rules.mp_session = true;
	ns::LoopbackChannel loop;
	inmatch::NapiNPServerCtx ctx;
	inmatch::GameConfig settings;
	settings.server_name = "DM";
	settings.max_players = 2;
	settings.game_type = 0;
	inmatch::test::bring_up_host(ctx, inmatch::ConnectionMode::HostClient,
			inmatch::SocketMode::Socketless, /*host_key=*/0, &loop, settings);
	ctx.world = world.get();
	MemoryFiles files;
	inmatch::ServerLogRecorder rec(files, "host");
	expect(rec.open("dm.bms"), "the /PROFILE recorder opens");
	ctx.logs.profile = &rec;
	inmatch::Server_InitNewRoundState(ctx);
	expect(inmatch::Server_ProcessPendingPlayerSpawns(ctx, *world) == 1, "the own add runs");
	rec.close();
	const std::string &text = files.files["host.sph"];
	const std::vector<uint8_t> file(text.begin(), text.end());
	ServerLogDocument doc;
	expect(decode_server_log(file.data(), file.size(), doc), "the /PROFILE file decodes");
	expect(doc.roster.empty(), "the own add files no roster row (the local arm @0x51CC60)");
}

void test_the_single_player_join_keeps_the_body_where_it_stands() {
	expect(join_x_after_moving(/*mp_session=*/false) == 5 * 65536,
			"single player: the join places nothing (is_in_session 0 @0x51A777)");
}

void test_the_session_join_places_the_body_again() {
	expect(join_x_after_moving(/*mp_session=*/true) == 100 * 65536,
			"session: the join's no-pick placement puts the body on the marker (@0x51A786)");
}

} // namespace

int main() {
	test_the_own_add_files_no_profile_row();
	test_the_single_player_join_keeps_the_body_where_it_stands();
	test_the_session_join_places_the_body_again();
	if (failures != 0) {
		std::fprintf(stderr, "%d failure(s)\n", failures);
		return 1;
	}
	std::printf("npruntime_sp_join_placement: ok\n");
	return 0;
}
