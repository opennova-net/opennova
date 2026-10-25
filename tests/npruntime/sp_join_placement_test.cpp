// The single player's join places nothing (D-NET-NEW-L1-19): retail's join re-places
// the body through the no-pick arm only in a session, and the single player runs with
// is_in_session 0, so its own body keeps the mission start's placement; in a session
// the join's placement moves the body to the start marker again.
// [orig: Server_OnPlayerJoin `cmp g_NapiNPCtx.is_in_session, ebx` @0x51A777, `jz`
//  @0x51A77D, the Server_PositionPlayerForSpawn call @0x51A786]
// The single player's join also sends its own loopback no 0x0A and leaves its slot's
// network words alone, both behind the same is_in_session test.
// [orig: Server_OnPlayerJoin `cmp g_NapiNPCtx.is_in_session, ebx` @0x51A824, `jz` @0x51A82A,
//  the Server_SendEntityStateToPlayer call @0x51A82D; PlayerSlot_ResetNetworkState @0x4F9FD0,
//  `cmp` @0x4F9FD3, `jz` @0x4F9FD9, the 744 countdown @0x4FA021]
// The single player's own loopback 0x08 tail and 0x0F flags carry no session bit: retail's
// BuildFlags returns 0 out of a session and the 0x0F's 0x02 bit (the target-less spawn
// restriction) needs is_in_session; ours reads the session fact off the socket mode at the
// create (BuildFlags) and off the world at the join (the 0x0F), as the join's other reads do.
// [orig: CNapiServerConfig_BuildFlags @0x4C4DC0 (`if (this->is_in_session)` @0x4C4DC4),
//  reached through Server_SendPlayerSlotStateFrame @0x51BF95 -> ServerConfig_SerializeToPacket
//  @0x505D29; NetPacket_WriteWorldStateLoad0x0F `cmp g_RespawnRequiresTeamDead, 0` @0x502DB2,
//  `cmp g_NapiNPCtx.is_in_session, 0` @0x502DBB, `or bl, 2` @0x502DC4, sent by
//  Server_OnPlayerJoin @0x51A869]
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
#include <runtime/inmatch/server_tick.h>
#include <net/npwire/ingame_message_id.h>
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
struct JoinResult {
	int32_t x = 0;
	int frames = 0;              // S2C 0x0A records among the join's sends
	uint32_t control_countdown = 0; // the slot's quartet countdown after the join
	int world_state_flags = -1;  // the join's 0x0F flags byte
	uint32_t build_flags = 0;    // the create's BuildFlags word (the 0x08 tail)
};

JoinResult join_after_moving(bool mp_session) {
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
	settings.default_spawn_requires_no_team_zone = 1; // g_RespawnRequiresTeamDead set
	// The shell's single player is socketless; a session host has a socket.
	inmatch::test::bring_up_host(ctx, inmatch::ConnectionMode::HostClient,
			mp_session ? inmatch::SocketMode::Lan : inmatch::SocketMode::Socketless,
			/*host_key=*/0, &loop, settings);
	ctx.world = world.get();
	inmatch::Server_InitNewRoundState(ctx);
	expect(inmatch::Server_ProcessPendingPlayerSpawns(ctx, *world) == 1, "the own add runs");
	expect(!world->cached.local_player.valid(), "the own add spawns no body (D-NET-NEW-L7-39)");
	inmatch::test::round_init_own_body(ctx); // the mission start's round init
	w::Entity *body = world->registry.get(world->cached.local_player);
	if (!expect(body != nullptr, "the round init spawns the local player")) return {};
	expect(body->position.x.q16 == 100 * 65536, "the round init places the body on the marker");
	body->position.x.q16 = 5 * 65536;
	inmatch::NapiNPConnection &own = ctx.np_protocol.connection_list.front();
	expect(own.slot_state != inmatch::PlayerSlotState::Joined, "the join is the slot's state-6 entry");
	own.reply.control_request_countdown = 5; // a word the state-6 entry's reset would reseed
	JoinResult out;
	out.build_flags = ctx.np_protocol.build_flags;
	for (const inmatch::InitialStateMessage &m : inmatch::Server_OnPlayerJoin(ctx, own, 1, {0, 0})) {
		out.frames += m.tag == s2c::PER_FRAME_UPDATE ? 1 : 0;
		// The flags byte follows the tick, the three position words and the three angles.
		if (m.tag == s2c::WORLD_STATE_LOAD && m.body.size() > 22) out.world_state_flags = m.body[22];
	}
	out.x = world->registry.get(world->cached.local_player)->position.x.q16;
	out.control_countdown = own.reply.control_request_countdown;
	return out;
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
	const JoinResult r = join_after_moving(/*mp_session=*/false);
	expect(r.x == 5 * 65536, "single player: the join places nothing (is_in_session 0 @0x51A777)");
	expect(r.frames == 0, "single player: the join sends its own loopback no 0x0A (@0x51A82A)");
	expect(r.control_countdown == 5,
			"single player: the state-6 entry resets no network word (@0x4F9FD9)");
	expect(r.build_flags == 0, "single player: BuildFlags is 0 out of a session (@0x4C4DC4)");
	expect(r.world_state_flags == 0,
			"single player: the 0x0F carries no target-less spawn bit (@0x502DBB)");
}

void test_the_session_join_places_the_body_again() {
	const JoinResult r = join_after_moving(/*mp_session=*/true);
	expect(r.x == 100 * 65536,
			"session: the join's no-pick placement puts the body on the marker (@0x51A786)");
	expect(r.frames == 1, "session: the join sends the slot one 0x0A ahead of the 0x0F (@0x51A82D)");
	expect(r.control_countdown == inmatch::CONTROL_REQUEST_PERIOD_TICKS,
			"session: the state-6 entry seeds the quartet countdown (@0x4FA021)");
	// A LAN session's word: the LAN bit and 0x800 (@0x4C4DF1, @0x4C4E04).
	expect((r.build_flags & 0x900u) == 0x900u, "session: BuildFlags carries the LAN session's bits");
	expect(r.world_state_flags == 0x02,
			"session: the 0x0F carries the target-less spawn bit (@0x502DC4)");
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
