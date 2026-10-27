// The join's S2C 0x02 padding probe (D-NET-968): its words are the host's GetTickCount,
// the process's file-transfer session id as it stands at the send and the padding length 0x100
// the client echoes, then 500 bytes of `rand() >> 3` drawn from the session's CRT stream; and
// that id's two writers: every authority mission start adds one and arms a 62-tick countdown,
// whose expiry at the server tick's head adds one more.
// [orig: CNetPlayer_SendFileTransferChunk @0x4C9440 (the id @0x4C9468, the size 0x200 @0x4C946E,
//  the extra word 0x100 @0x4C9459) -> NetPacket_BuildPaddedTimestampPayload @0x502A40
//  (GetTickCount @0x502A4E, +4 @0x502A76, +8 @0x502A82, `rand() >> 3` @0x502A9D, the loop
//  @0x502A93..0x502AAF); g_FileTransferSessionId @0xC86FCC and
//  g_FileTransferSessionStepCountdown @0xC86FD0 (both zero in the image); Game_StartMission
//  @0x524360 (`add g_FileTransferSessionId, ebp` @0x5244B4, ebp = 1 @0x524388; the store of 62
//  @0x5244C6); Server_TickUpdate @0x51D7E0 (the countdown @0x51D809..0x51D81E, the add
//  @0x51D820)]

#include <base/gameprofile/game_type.h>
#include <base/gameprofile/gameprofile.h>
#include <base/io/crt_rand.h>
#include <base/io/le.h>
#include <net/npwire/ingame_message_id.h>
#include <net/title/title_identity.h>
#include <runtime/inmatch/client_runtime.h> // ClientProcessState
#include <runtime/inmatch/host_role.h>
#include <runtime/inmatch/host_session.h>
#include <runtime/inmatch/napi_np_connection.h>
#include <runtime/inmatch/napi_np_server_ctx.h>
#include <runtime/inmatch/server_dispatch_join.h>
#include <runtime/inmatch/server_tick.h> // Server_TickUpdate
#include <runtime/mission/mission_kernel.h>
#include <runtime/world/world.h>

#include "../common/boot_file_source.h"
#include "../common/synthetic_mission.h"

#include <cstdio>
#include <map>
#include <memory>
#include <string>

using namespace opennova;
namespace ms = opennova::mission;

namespace {

int g_failures = 0;

#define CHECK(cond)                                                                    \
	do {                                                                               \
		if (!(cond)) {                                                                 \
			std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);    \
			++g_failures;                                                              \
		}                                                                              \
	} while (0)

// The probe over a host context: our GetTickCount (the start's stamp plus the run's
// milliseconds), the context's id, 0x100, then the world stream's next 500 draws, each
// `>> 3` and cut to its low byte; the stream stands 500 draws on; the connection keeps the two
// words the echo must return and awaits it.
void test_probe_words() {
	auto world = std::make_unique<world::World>();
	world->crt_rand.seed(0x1234u);
	auto ctx = std::make_unique<inmatch::NapiNPServerCtx>();
	ctx->world = world.get();
	ctx->np_protocol.host_start_tick = 100000u;
	ctx->np_protocol.host_run_duration_ms = 3216u;
	inmatch::FileTransferSession words;
	words.id = 3u;
	ctx->file_transfer_session = &words;
	auto conn = std::make_unique<inmatch::NapiNPConnection>();
	const ProtocolMessage probe = inmatch::admission_padding_probe(*conn, ctx.get());
	CHECK(probe.tag == s2c::JOIN_PADDING_PROBE);
	CHECK(probe.payload.size() == 512u);
	if (probe.payload.size() != 512u) return;
	CHECK(io::read_u32_le(probe.payload.data()) == 103216u);
	CHECK(io::read_u32_le(probe.payload.data() + 4) == 3u);
	CHECK(io::read_u32_le(probe.payload.data() + 8) == 0x100u);
	io::CrtRand expected;
	expected.seed(0x1234u);
	bool padding = true;
	for (size_t i = 12; i < 512; ++i)
		padding = padding && probe.payload[i] == static_cast<uint8_t>(expected.next() >> 3);
	CHECK(padding);
	CHECK(world->crt_rand.state == expected.state);
	CHECK(conn->admission_stage == inmatch::GameAdmissionStage::AwaitPaddingEcho);
	// A worked byte: the seed 0x1234's first draw is (0x1234 * 214013 + 2531011) mod 2^32 =
	// 0x3B983827, >> 16 & 0x7FFF = 0x3B98, >> 3 = 0x773, whose low byte 0x73 is the padding's first.
	CHECK(probe.payload[12] == 0x73u);
}

const TitleIdentity &kJo = *title_identity(gameprofile::GAME_JO);

// One authority mission start over a fresh kernel, a listen host whose process words are
// `process`, kept standing so its server ticks can run (the role, declared last, goes first).
struct HostMission {
	std::unique_ptr<ms::MissionKernel> kernel = std::make_unique<ms::MissionKernel>();
	std::unique_ptr<inmatch::HostRole> role = std::make_unique<inmatch::HostRole>();
	inmatch::NapiNPServerCtx &ctx() { return role->state.host_owner.ctx; }
};

std::unique_ptr<HostMission> start_mission(inmatch::ClientProcessState &process,
		std::map<std::string, std::string> &files) {
	auto mission = std::make_unique<HostMission>();
	ms::MissionKernel &kernel = *mission->kernel;
	inmatch::HostRole &role = *mission->role;
	role.bind(kernel);
	kernel.open_document(test_mission::two_entity_mission(), "probe_words", test_boot::source_over(&files));
	ms::KernelBootOptions options;
	options.mp_session = true;
	options.game_type = game_type::for_mission_attribs(kernel.mission.header.attrib_flags);
	options.bringup_net_session = [&] {
		inmatch::HostConfig cfg;
		cfg.title = &kJo;
		cfg.config.server_name = "probe_words";
		cfg.config.max_players = 4;
		cfg.config.game_type = options.game_type;
		cfg.socket_mode = inmatch::SocketMode::Lan;
		cfg.serve_and_play = true;
		cfg.client_process = &process;
		inmatch::HostBringup bringup;
		bringup.host_cfg = cfg;
		role.bring_up(bringup);
	};
	std::string error;
	CHECK(kernel.boot(options, error));
	return mission;
}

// The id word of the probe the context would send now.
uint32_t probe_id(inmatch::NapiNPServerCtx &ctx) {
	auto conn = std::make_unique<inmatch::NapiNPConnection>();
	const ProtocolMessage probe = inmatch::admission_padding_probe(*conn, &ctx);
	return probe.payload.size() == 512u ? io::read_u32_le(probe.payload.data() + 4) : 0xFFFFFFFFu;
}

// The process words: zero in the image; each authority mission start of the process (a later
// session's included) adds one and arms the countdown at 62; the 62nd server tick after it adds
// one more, and later ticks none; the probe reads the id as it stands. So a fresh host probes
// with 1 inside its first mission's 62 server ticks, 2 after them, and 3, then 4, in the next.
void test_mission_start_counter() {
	std::map<std::string, std::string> files;
	inmatch::ClientProcessState process;
	const inmatch::FileTransferSession &words = process.file_transfer_session;
	CHECK(words.id == 0u && words.step_countdown == 0);
	{
		const std::unique_ptr<HostMission> first = start_mission(process, files);
		inmatch::NapiNPServerCtx &ctx = first->ctx();
		CHECK(ctx.file_transfer_session == &process.file_transfer_session);
		CHECK(words.id == 1u && words.step_countdown == 62);
		CHECK(probe_id(ctx) == 1u);
		for (int i = 0; i < 61; ++i) inmatch::Server_TickUpdate(ctx);
		CHECK(words.id == 1u && words.step_countdown == 1);
		CHECK(probe_id(ctx) == 1u);
		inmatch::Server_TickUpdate(ctx);
		CHECK(words.id == 2u && words.step_countdown == 0);
		CHECK(probe_id(ctx) == 2u);
		for (int i = 0; i < 124; ++i) inmatch::Server_TickUpdate(ctx);
		CHECK(words.id == 2u && words.step_countdown == 0);
	}
	{
		const std::unique_ptr<HostMission> second = start_mission(process, files);
		inmatch::NapiNPServerCtx &ctx = second->ctx();
		CHECK(words.id == 3u && words.step_countdown == 62);
		CHECK(probe_id(ctx) == 3u);
		for (int i = 0; i < 62; ++i) inmatch::Server_TickUpdate(ctx);
		CHECK(words.id == 4u && words.step_countdown == 0);
		CHECK(probe_id(ctx) == 4u);
	}
}

// The countdown alone: a signed test ahead of the decrement, so a word at zero or below is left
// as it stands. [orig: Server_TickUpdate @0x51D7E0 (`cmp` @0x51D809, `jle` @0x51D811)]
void test_step_countdown() {
	inmatch::FileTransferSession words;
	words.step_server_tick();
	CHECK(words.id == 0u && words.step_countdown == 0);
	words.step_countdown = -5;
	words.step_server_tick();
	CHECK(words.id == 0u && words.step_countdown == -5);
	words.step_countdown = 1;
	words.step_server_tick();
	CHECK(words.id == 1u && words.step_countdown == 0);
}

} // namespace

int main() {
	test_probe_words();
	test_mission_start_counter();
	test_step_countdown();
	if (g_failures != 0) {
		std::fprintf(stderr, "npruntime_join_probe_words: %d failure(s)\n", g_failures);
		return 1;
	}
	std::printf("npruntime_join_probe_words: ok\n");
	return 0;
}
