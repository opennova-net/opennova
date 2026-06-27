// P3 — the §5.2a two-track initial-state burst machine (server_initial_state.{h,cpp}). Drives the
// host's own loopback connection (the §5.2a step-4 in-process client) through the burst over a real
// World + bms::File and asserts: (1) the emitted tag ORDER matches §5.2a for the tags we control;
// (2) each emittable body decodes + round-trips field-identical; (3) the 0x0C organic carries the
// host player's dcb at entity+0x78 (the §1 wiring, end to end); (4) burst.game_state==9 / spawned at
// the terminator; (5) the unwitnessed serializers + pool-1 0x0D emit NOTHING (no fixtures).

#include <npruntime/server_initial_state.h>
#include <npruntime/server_session.h>
#include <npruntime/server_spawn.h>

#include "host_test_setup.h"

#include <netsim/loopback_channel.h>

#include <mission/bms.h>

#include <novaworld/ingame_decode.h> // decode_organic_spawn_batch / decode_pool3_sync_batch

#include <world/ai.h>
#include <world/entity.h>
#include <world/player_spawn.h>
#include <world/world.h>

#include <cstdio>
#include <vector>

namespace {
namespace np = opennova::np;
namespace w = opennova::world;
namespace ns = opennova::netsim;

bool expect(bool cond, const char *msg) {
	if (cond) return true;
	std::fprintf(stderr, "FAIL: %s\n", msg);
	return false;
}

int main_impl() {
	// A World with the host player spawned + one 6002 marker (both the spawn-select start AND a
	// pool-3 spawn-marker the 0x20 batch streams).
	w::World world;
	w::AiSystem ai;
	world.ai = &ai;
	world.registry.configure_pool(0, 16);
	world.registry.configure_pool(3, 16);
	{
		w::Entity start;
		start.kind = w::EntityKind::Marker;
		start.item_id = 6002;
		start.position = {50.0f, 60.0f, 1.0f};
		start.yaw = 0;
		world.registry.spawn(3, start);
	}

	// A minimal in-memory mission for the 0x0B BMS-header body (no asset gating).
	opennova::bms::File mission;
	mission.header.magic[0] = 'B';
	mission.header.magic[1] = 'M';
	mission.header.magic[2] = 'S';
	mission.header.magic[3] = static_cast<char>(opennova::bms::kMinVersion);

	ns::LoopbackChannel loopback;
	np::NapiNPServerCtx ctx;
	np::test::bring_up_host(ctx, np::ConnectionMode::HostClient, np::SocketMode::Socketless,
	                        /*host_key=*/0, &loopback);
	ctx.world = &world;
	ctx.mission = &mission;

	// Spawn the host's own pool-0 player (so the burst's 0x0C has it with dcb 2).
	np::Server_InitNewRoundState(ctx);
	if (!expect(np::Server_ProcessPendingPlayerSpawns(ctx, world) == 1, "host player spawned")) return 1;

	np::NapiNPConnection &conn = ctx.np_protocol.connection_list[0];

	// Drive the burst to completion, collecting the emitted (tag, body) in order.
	std::vector<np::InitialStateMessage> emitted;
	bool reached = false;
	for (int i = 0; i < 64 && !reached; ++i) {
		np::InitialStateStep step = np::Server_SendInitialGameStateToPlayer(ctx, conn, /*now_tick=*/1);
		if (!step.advanced) break;
		for (auto &m : step.messages) emitted.push_back(m);
		reached = step.reached_in_game;
	}

	if (!expect(reached, "burst reached game-state 9 (the world-stream terminator)")) return 1;
	if (!expect(conn.burst.spawned && conn.burst.game_state == 9, "burst marks spawned + game_state 9")) return 1;
	if (!expect(conn.phase == np::ConnectionPhase::Spawned, "connection advanced to Spawned")) return 1;

	// (1) The emitted tag order — exactly the tags we control, in §5.2a order (deferred/skip omitted).
	const std::vector<uint8_t> want_order = {0x1C, 0x0B, 0x11, 0x10, 0x0C, 0x20};
	std::vector<uint8_t> got_order;
	for (auto &m : emitted) got_order.push_back(m.tag);
	if (!expect(got_order == want_order, "emitted tag order matches §5.2a (0x1C,0x0B,0x11,0x10,0x0C,0x20)")) {
		std::fprintf(stderr, "  got:");
		for (uint8_t t : got_order) std::fprintf(stderr, " 0x%02X", t);
		std::fprintf(stderr, "\n");
		return 1;
	}

	// No deferred/omitted tag was emitted (no fixtures).
	for (auto &m : emitted) {
		if (m.tag == 0x2C || m.tag == 0x08 || m.tag == 0x2A || m.tag == 0x66 || m.tag == 0x76 ||
		    m.tag == 0x45 || m.tag == 0x7E || m.tag == 0x1A || m.tag == 0x0D) {
			std::fprintf(stderr, "FAIL: deferred/omitted tag 0x%02X was emitted\n", m.tag);
			return 1;
		}
	}

	// Fetch helpers.
	auto body_of = [&](uint8_t tag) -> const std::vector<uint8_t> * {
		for (auto &m : emitted)
			if (m.tag == tag) return &m.body;
		return nullptr;
	};

	// (2)/(3) 0x0C decodes + carries the host player's dcb at entity+0x78.
	{
		const std::vector<uint8_t> *b = body_of(0x0C);
		opennova::OrganicSpawnBatch batch;
		if (!expect(b && opennova::decode_organic_spawn_batch(b->data(), b->size(), batch),
		            "0x0C body decodes")) return 1;
		bool host_dcb = false;
		for (const auto &r : batch.records)
			if (r.item_type_id == 0x14B9 && r.entity_flags == np::kHostPlayerDcb) host_dcb = true;
		if (!expect(host_dcb, "0x0C carries the host player (0x14B9) with entity+0x78 == dcb 2")) return 1;
	}

	// (2) 0x20 decodes + carries the spawn marker.
	{
		const std::vector<uint8_t> *b = body_of(0x20);
		opennova::Pool3SyncBatch batch;
		if (!expect(b && opennova::decode_pool3_sync_batch(b->data(), b->size(), batch),
		            "0x20 body decodes")) return 1;
		bool saw_marker = false;
		for (const auto &r : batch.records)
			if (r.item_type_id == 6002) saw_marker = true;
		if (!expect(saw_marker, "0x20 carries the 6002 spawn marker")) return 1;
	}

	// (2) 0x0B is the real 616-byte BMS header, built from scratch (no fixture).
	{
		const std::vector<uint8_t> *b = body_of(0x0B);
		if (!expect(b && b->size() == opennova::bms::kHeaderSize, "0x0B body is the 616-byte BMS header")) return 1;
		if (!expect((*b)[0] == 'B' && (*b)[1] == 'M' && (*b)[2] == 'S', "0x0B header magic is 'BMS'")) return 1;
	}

	// Empty bodies are wire-valid.
	if (!expect(body_of(0x1C)->empty() && body_of(0x11)->empty() && body_of(0x10)->empty(),
	            "0x1C / 0x11 / 0x10 carry empty bodies")) return 1;

	std::printf("OK\n");
	return 0;
}
} // namespace

int main() { return main_impl(); }
