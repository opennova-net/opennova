// P3 — the §5.2a two-track initial-state burst machine (server_initial_state.{h,cpp}). Drives the
// host's own loopback connection (the §5.2a step-4 in-process client) through the burst over a real
// World + bms::File + host config and asserts: (1) the full §5.2a emitted tag ORDER; (2) each body
// decodes / round-trips / byte-matches the golden-witnessed value (incl. the 0x2C/0x08/0x2A/0x66/0x76/
// 0x1A serializers ported 2026-06-27); (3) the 0x0C organic carries the host player's dcb at
// entity+0x78 (the §1 wiring, end to end); (4) burst.game_state==9 / spawned at the terminator;
// (5) the pool-1 0x0D + the conditional terrain/briefing tags stay absent (faithful, no fixtures).

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

	// (1) The full §5.2a emitted tag order. Player-sync: 0x2C, 0x08, 0x2A×6, 0x1C, 0x0B, 0x66, 0x76,
	// 0x11 (matches the retail-lan-host-join golden frames 144-160). World-stream: 0x10, 0x0C, 0x20,
	// 0x1A (0x0D pool-1 + 0x45 terrain + 0x7E briefing are faithfully absent — D-NET-97/98 + the
	// witnessed conditional empties).
	const std::vector<uint8_t> want_order = {0x2C, 0x08, 0x2A, 0x2A, 0x2A, 0x2A, 0x2A, 0x2A,
	                                         0x1C, 0x0B, 0x66, 0x76, 0x11, 0x10, 0x0C, 0x20, 0x1A};
	std::vector<uint8_t> got_order;
	for (auto &m : emitted) got_order.push_back(m.tag);
	if (!expect(got_order == want_order, "emitted tag order matches §5.2a (full player-sync + world-stream)")) {
		std::fprintf(stderr, "  got:");
		for (uint8_t t : got_order) std::fprintf(stderr, " 0x%02X", t);
		std::fprintf(stderr, "\n");
		return 1;
	}

	// The pool-1 0x0D and the conditional terrain/briefing tags stay absent (no fixtures, faithful).
	for (auto &m : emitted) {
		if (m.tag == 0x0D || m.tag == 0x45 || m.tag == 0x7E) {
			std::fprintf(stderr, "FAIL: conditionally-absent tag 0x%02X was emitted\n", m.tag);
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

	// Empty scalar bodies are wire-valid for 0x1C/0x11, but 0x10 is a static-entity batch and even
	// an empty batch must carry its [u16 start_index][u16 count] header.
	if (!expect(body_of(0x1C)->empty() && body_of(0x11)->empty(),
	            "0x1C / 0x11 carry empty bodies")) return 1;
	{
		const std::vector<uint8_t> *b = body_of(0x10);
		opennova::StaticEntityBatch batch;
		if (!expect(b && opennova::decode_static_entity_batch(b->data(), b->size(), batch),
		            "0x10 empty static batch decodes")) return 1;
		if (!expect(b->size() == 4 && batch.start_index == 0 && batch.entity_count == 0 &&
		                    batch.records.empty(),
		            "0x10 empty static batch carries the 4-byte header")) return 1;
	}

	// (6) The §5.2a serializers ported from IDA (grilled 2026-06-27). Bodies cross-checked vs the
	// retail-lan-host-join golden (frames 144-160).
	{
		// 0x2C = server name + mission file, two NUL-terminated C strings (from session_config).
		const std::vector<uint8_t> *b = body_of(0x2C);
		const std::string sn = ctx.session_config.server_name, mf = ctx.session_config.mission_file;
		std::vector<uint8_t> want;
		want.insert(want.end(), sn.begin(), sn.end()); want.push_back(0);
		want.insert(want.end(), mf.begin(), mf.end()); want.push_back(0);
		if (!expect(b && *b == want, "0x2C = serverName\\0 + missionFile\\0")) return 1;
	}
	{
		// 0x08 = the 51-byte server-config block (10 rule dwords default 0 + 7 bytes + flags dword).
		const std::vector<uint8_t> *b = body_of(0x08);
		if (!expect(b && b->size() == 51, "0x08 server-config is 51 bytes")) return 1;
		bool dwords_zero = true; // default ServerRules -> all 10 rule dwords 0
		for (int i = 0; i < 40; ++i) dwords_zero = dwords_zero && ((*b)[i] == 0);
		if (!expect(dwords_zero, "0x08 default rule dwords are 0")) return 1;
	}
	{
		// 0x2A ×6 — each the const table record {00 04 b0 ab b2 b2 bf bc bd ba} (golden frames 148-158).
		static const std::vector<uint8_t> kRec = {0x00, 0x04, 0xb0, 0xab, 0xb2, 0xb2, 0xbf, 0xbc, 0xbd, 0xba};
		int count_2a = 0;
		for (auto &m : emitted) {
			if (m.tag != 0x2A) continue;
			++count_2a;
			if (!expect(m.body == kRec, "0x2A record == const table payload")) return 1;
		}
		if (!expect(count_2a == 6, "exactly six 0x2A records emitted")) return 1;
	}
	{
		// 0x66 weapon-restrictions: no restrictions -> single count byte 0 (golden frame 160).
		const std::vector<uint8_t> *b = body_of(0x66);
		if (!expect(b && b->size() == 1 && (*b)[0] == 0, "0x66 empty restriction table = {0}")) return 1;
		// 0x76 server-tick16 = now_tick low 16 (now_tick=1 -> 01 00).
		const std::vector<uint8_t> *t = body_of(0x76);
		if (!expect(t && *t == std::vector<uint8_t>{0x01, 0x00}, "0x76 server-tick16 = now_tick low16")) return 1;
		// 0x1A timestamp = now_tick u32 (now_tick=1 -> 01 00 00 00).
		const std::vector<uint8_t> *ts = body_of(0x1A);
		if (!expect(ts && *ts == std::vector<uint8_t>{0x01, 0x00, 0x00, 0x00}, "0x1A timestamp = now_tick u32")) return 1;
	}

	// --- Regression (D-NET-114 one-shot burst F3 latch): driving the World path through
	// tick_connections must surface BOTH PeerEnteredWorldStreaming (F3) AND PeerSpawned, with F3 first.
	// The one-shot drain latches entity_batch_count and burst.spawned in the SAME call, so a
	// !spawned-gated F3 predicate would drop F3 entirely on the World path (losing the dcb-timing
	// signal). This drives a fresh host loopback (unspawned) so tick_connections runs the full burst. ---
	{
		w::World w2;
		w::AiSystem ai2;
		w2.ai = &ai2;
		w2.registry.configure_pool(0, 16);
		w2.registry.configure_pool(3, 16);
		{
			w::Entity m;
			m.kind = w::EntityKind::Marker;
			m.item_id = 6002;
			m.position = {10.0f, 20.0f, 1.0f};
			w2.registry.spawn(3, m);
		}
		ns::LoopbackChannel lb2;
		np::NapiNPServerCtx ctx2;
		np::test::bring_up_host(ctx2, np::ConnectionMode::HostClient, np::SocketMode::Socketless,
		                        /*host_key=*/0, &lb2);
		ctx2.world = &w2;
		ctx2.mission = &mission;

		const std::vector<np::TickOut> outs = np::tick_connections(ctx2, /*elapsed_ms=*/16, /*now_tick=*/1);
		int f3_at = -1, spawned_at = -1, seq = 0;
		for (const np::TickOut &to : outs) {
			for (const np::HostAcceptEvent &ev : to.events) {
				if (ev.kind == np::HostAcceptEvent::Kind::PeerEnteredWorldStreaming) {
					if (!expect(ev.self_id == np::kHostPlayerDcb, "F3 self_id == host dcb")) return 1;
					if (f3_at < 0) f3_at = seq;
				} else if (ev.kind == np::HostAcceptEvent::Kind::PeerSpawned) {
					if (!expect(ev.self_id == np::kHostPlayerDcb, "PeerSpawned self_id == host dcb")) return 1;
					if (spawned_at < 0) spawned_at = seq;
				}
				++seq;
			}
		}
		if (!expect(f3_at >= 0, "one-shot World burst still surfaces PeerEnteredWorldStreaming (F3)")) return 1;
		if (!expect(spawned_at >= 0, "one-shot World burst surfaces PeerSpawned")) return 1;
		if (!expect(f3_at < spawned_at, "F3 surfaces BEFORE PeerSpawned (dcb-timing order)")) return 1;
	}

	std::printf("OK\n");
	return 0;
}
} // namespace

int main() { return main_impl(); }
