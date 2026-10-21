// The join's S2C 0x02 padding probe (D-NET-NEW-L8-20): its words are the host's GetTickCount,
// the process's file-transfer session id as the mission start stepped it and the padding length
// 0x100 the client echoes, then 500 bytes of `rand() >> 3` drawn from the session's CRT stream;
// and that id's process counter, which every authority mission start steps by one.
// [orig: CNetPlayer_SendFileTransferChunk @0x4C9440 (the id @0x4C9468, the size 0x200 @0x4C946E,
//  the extra word 0x100 @0x4C9459) -> NetPacket_BuildPaddedTimestampPayload @0x502A40
//  (GetTickCount @0x502A4E, +4 @0x502A76, +8 @0x502A82, `rand() >> 3` @0x502A9D, the loop
//  @0x502A93..0x502AAF); g_FileTransferSessionId @0xC86FCC (zero in the image), stepped by
//  Game_StartMission @0x5244B4 (`add g_FileTransferSessionId, ebp`, ebp = 1 @0x524388)]

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
	ctx->file_transfer_session_id = 3u;
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
	CHECK(conn->admission_padding_x == 103216u && conn->admission_padding_y == 3u);
	CHECK(conn->admission_stage == inmatch::GameAdmissionStage::AwaitPaddingEcho);
	// A worked byte: the seed 0x1234's first draw is (0x1234 * 214013 + 2531011) mod 2^32 =
	// 0x3B983827, >> 16 & 0x7FFF = 0x3B98, >> 3 = 0x773, whose low byte 0x73 is the padding's first.
	CHECK(probe.payload[12] == 0x73u);
}

const TitleIdentity &kJo = *title_identity(gameprofile::GAME_JO);

// One authority mission start over a fresh kernel, a listen host whose process words are
// `process`: the id its context took.
uint32_t mission_start_id(inmatch::ClientProcessState &process,
		std::map<std::string, std::string> &files) {
	auto kernel_owner = std::make_unique<ms::MissionKernel>();
	auto role_owner = std::make_unique<inmatch::HostRole>();
	ms::MissionKernel &kernel = *kernel_owner;
	inmatch::HostRole &role = *role_owner;
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
	return role.state.host_owner.ctx.file_transfer_session_id;
}

// The process counter: zero in the image, one more at each authority mission start of the
// process (a later session's included); a role with no process words of the embedder's counts
// on its own.
void test_mission_start_counter() {
	std::map<std::string, std::string> files;
	inmatch::ClientProcessState process;
	CHECK(process.file_transfer_session_id == 0u);
	CHECK(mission_start_id(process, files) == 1u);
	CHECK(mission_start_id(process, files) == 2u);
	CHECK(process.file_transfer_session_id == 2u);
}

} // namespace

int main() {
	test_probe_words();
	test_mission_start_counter();
	if (g_failures != 0) {
		std::fprintf(stderr, "npruntime_join_probe_words: %d failure(s)\n", g_failures);
		return 1;
	}
	std::printf("npruntime_join_probe_words: ok\n");
	return 0;
}
