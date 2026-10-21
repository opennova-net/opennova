// D-NET-NEW-L1-21: the host's C2S 0x01 handler. Retail answers a 0x01 only for a net player in
// game state 2 (the JOIN accepted) with no server slot; there it runs the side passwords' second
// test, whatever the team-choose rule the 0x00's validator follows: with either password set, a
// joiner that is no spectator (JSR 0) and whose JSP equals neither password, case-insensitively,
// is rejected with reason 17 and gets no probe (an empty JSP equals an unset password). The
// handler reads no payload. D-NET-NEW-L1-24: a 0x01 out of turn (ahead of the JOIN, or past the
// form post) and a JOIN out of turn (past game state 1) answer nothing, draw nothing from the
// CRT stream and keep the node in its stage. The 0x02's handler has no gate and no echo test:
// any 0x02 completes the admission (its records, game state 7), ahead of the JOIN too, and
// again at a completed one; each admission record of a datagram reaches its handler in order,
// the records after the completing one the in-match switch. A game record ahead of the admission
// reaches its handler, whose server-slot gate (+0xC0, set only by the add) answers nothing: the
// node stays (the 0x0A, 0x2F and 0x0B pinned).
// [orig: NapiNPServerMsg_HandleJoinRequest @0x512ED0 - `cmp [edi+0A0h], 2` @0x512EF1,
//  `cmp [edi+0C0h], 0` @0x512EFE, the two lengths @0x512F0B..0x512F3B, the JSR test @0x512F3D,
//  _stricmp(JSP, A) @0x512F52 and (JSP, B) @0x512F64, CNapiNPConnection_QueueDisconnectEvent
//  (player, "", 17, "") @0x512F7E; state 6 @0x512FB3 and the probe's call @0x512FBC;
//  NapiNPServer_HandlePlayerJoinMessage @0x512AA0 (`cmp [ebx+0A0h], 1` @0x512AF9 / `jnz`
//  @0x512B00 to the return @0x512EAC; state 2 @0x512E7D);
//  NapiNPServerMsg_0x002 @0x512FD0 (is_authority @0x512FFC, the net player @0x51300B, the sends
//  @0x5130AC / @0x5130E5 / @0x51311E, game state 7 @0x51319C, the 0x03 @0x5131D5);
//  CNetPlayer_SetGameState @0x4C4060 (leaving 10 @0x4C4135; case 7 zeroes +0x24 @0x4C4184, the
//  spawn pump's hold-nag stamp, tested @0x4C8F7A..0x4C8F7F);
//  NapiNPServerMsg_HandlePlayerSpawnRequest @0x513260 (the slot @0x513280..0x513288);
//  NapiNPServerMsg_PlayerJoinRequest @0x51AB10 (the slot @0x51AB30..0x51AB38);
//  NapiNPServerMsg_HandlePlayerLoadout @0x515790 (the slot @0x5157CF..0x5157DB);
//  Server_ValidatePlayerJoinRequest @0x512100 (its side-password leg only under BuildFlags & 4,
//  `test al, 4` @0x51249A, with JSR 0 @0x5124A2)]

#include "npruntime/conn_fixture.h"

#include <base/io/crt_rand.h>
#include <net/npwire/ingame_message_id.h>
#include <net/npwire/protocol_message.h>
#include <net/npwire/session_hello.h>
#include <runtime/inmatch/loopback_channel.h>
#include <runtime/inmatch/napi_np_server_ctx.h>
#include <runtime/inmatch/server_message_dispatch.h>
#include <runtime/world/world.h>

#include <cstdio>
#include <memory>
#include <string>
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

struct Case {
	std::string side_a;
	std::string side_b;
	std::string jsp;
	uint8_t jsr = 0;
	bool probes = false; // else rejected with 17
	std::vector<uint8_t> body{0x00}; // the 0x01's body, which the handler never reads
};

// One joiner in the form-post state (the JOIN accepted), no team-choose rule, a 0x01 read.
void run_form_post(const Case &c) {
	auto world = std::make_unique<w::World>(); // heap: 1 MB stack
	world->crt_rand.seed(0x1234u);
	inmatch::NapiNPServerCtx ctx;
	ctx.is_authority = 1;
	ctx.is_in_session = 1;
	ctx.transport_mode = inmatch::NetworkType::Lan;
	ctx.world = world.get();
	ctx.config.mp_attributes = 0; // no team choose: the 0x00's validator tests no side password
	ctx.config.side_a_password = c.side_a;
	ctx.config.side_b_password = c.side_b;
	ns::LoopbackChannel wire;
	ctx.np_protocol.connection_list.push_back(conn_fixture::make_conn(
			20, inmatch::NapiNPConnection::kTypeServerSide, &wire, ns::TransportMode::Client,
			w::EntityHandle{}, false));
	inmatch::NapiNPConnection &conn = ctx.np_protocol.connection_list.front();
	conn.admission_stage = inmatch::GameAdmissionStage::AwaitFormPost;
	conn.join_password = c.jsp;
	conn.join_spectator_request = c.jsr;
	inmatch::ServerDispatchInputs inputs;
	inputs.server_ctx = &ctx;
	const io::CrtRand before = world->crt_rand;
	const std::vector<ProtocolMessage> replies = inmatch::dispatch_session_replies(ctx.config,
			conn, {make_protocol_message(0x01, c.body)}, 7, ctx.np_protocol.connection_list,
			world.get(), inputs);
	ns::Datagram d;
	if (c.probes) {
		CHECK(replies.size() == 1 && replies[0].tag == s2c::JOIN_PADDING_PROBE);
		CHECK(conn.admission_stage == inmatch::GameAdmissionStage::AwaitPaddingEcho);
		CHECK(!conn.host_disconnect_sent);
		CHECK(!wire.client_recv(d));
		io::CrtRand spent = before;
		for (int i = 0; i < 500; ++i) (void)spent.next();
		CHECK(world->crt_rand.state == spent.state);
	} else {
		// QueueDisconnectEvent from the server: DS 1, DC 2, reason 17, both strings empty.
		DisconnectEvent reject;
		reject.ds = 1;
		reject.dc = 2;
		reject.dpc = 17;
		CHECK(replies.empty());
		CHECK(conn.admission_stage == inmatch::GameAdmissionStage::Rejected);
		CHECK(conn.host_disconnect_sent);
		CHECK(wire.client_recv(d) && d.tag == hightag::DESCRIPTION_PACKET &&
				d.body == connection_description_to_bytes(reject));
		CHECK(!wire.client_recv(d));
		CHECK(world->crt_rand.state == before.state);
	}
}

void test_the_side_passwords_second_test() {
	run_form_post({"", "", "", 0, true});              // no password set
	run_form_post({"", "", "anything", 0, true});      // no password set: the JSP is not read
	run_form_post({"Alpha", "", "aLPHA", 0, true});    // A matches, case-insensitively
	run_form_post({"Alpha", "Bravo", "bravo", 0, true}); // B matches
	run_form_post({"Alpha", "", "wrong", 0, false});   // set, matched by neither: 17
	run_form_post({"Alpha", "", "", 0, true});         // an empty JSP equals the unset B
	run_form_post({"Alpha", "Bravo", "", 0, false});   // both set, an empty JSP equals neither: 17
	run_form_post({"", "Bravo", "", 0, true});         // an empty JSP equals the unset A
	run_form_post({"", "Bravo", "wrong", 0, false});   // a JSP equals neither the unset A nor B
	run_form_post({"Alpha", "Bravo", "wrong", 1, true}); // a spectator request skips the test
	run_form_post({"", "", "", 0, true, {}});          // the body is not read: empty
	run_form_post({"", "", "", 0, true, {0x07, 0x01}}); // or anything
}

// A JOIN or a 0x01 out of its handler's game state: nothing sent, nothing drawn, the node kept
// in its stage. A completed admission is state 6 on, so a repeated 0x01 or JOIN is one of these.
void run_out_of_turn(inmatch::GameAdmissionStage stage, uint8_t tag) {
	auto world = std::make_unique<w::World>(); // heap: 1 MB stack
	world->crt_rand.seed(0x1234u);
	inmatch::NapiNPServerCtx ctx;
	ctx.is_authority = 1;
	ctx.is_in_session = 1;
	ctx.transport_mode = inmatch::NetworkType::Lan;
	ctx.world = world.get();
	ns::LoopbackChannel wire;
	ctx.np_protocol.connection_list.push_back(conn_fixture::make_conn(
			20, inmatch::NapiNPConnection::kTypeServerSide, &wire, ns::TransportMode::Client,
			w::EntityHandle{}, false));
	inmatch::NapiNPConnection &conn = ctx.np_protocol.connection_list.front();
	conn.admission_stage = stage;
	inmatch::ServerDispatchInputs inputs;
	inputs.server_ctx = &ctx;
	const uint32_t before = world->crt_rand.state;
	const std::vector<ProtocolMessage> replies = inmatch::dispatch_session_replies(ctx.config,
			conn, {make_protocol_message(tag, {0x00})}, 7, ctx.np_protocol.connection_list,
			world.get(), inputs);
	CHECK(replies.empty());
	CHECK(world->crt_rand.state == before);
	CHECK(conn.admission_stage == stage);
	CHECK(!conn.host_disconnect_sent);
	ns::Datagram d;
	CHECK(!wire.client_recv(d));
}

void test_out_of_turn_join_records_answer_nothing() {
	using S = inmatch::GameAdmissionStage;
	for (const S stage : {S::AwaitJoinRequest, S::AwaitPlayEnter, S::AwaitPaddingEcho, S::Complete})
		run_out_of_turn(stage, 0x01);
	for (const S stage : {S::AwaitFormPost, S::AwaitPlayEnter, S::AwaitPaddingEcho, S::Complete})
		run_out_of_turn(stage, 0x00);
}


// One admission datagram over a fresh node at `stage`: the client-side node skips the remote
// validator, so a bare JOIN is accepted.
struct AdmissionRun {
	std::vector<ProtocolMessage> replies;
	inmatch::GameAdmissionStage stage = inmatch::GameAdmissionStage::AwaitJoinRequest;
	uint8_t game_state = 0;
	uint32_t watermark = 0;
	int draws = 0;
};

AdmissionRun run_admission(inmatch::GameAdmissionStage stage,
		const std::vector<ProtocolMessage> &records, uint8_t game_state = 0,
		uint32_t watermark = 0) {
	auto world = std::make_unique<w::World>(); // heap: 1 MB stack
	world->crt_rand.seed(0x1234u);
	inmatch::NapiNPServerCtx ctx;
	ctx.is_authority = 1;
	ctx.is_in_session = 1;
	ctx.transport_mode = inmatch::NetworkType::Lan;
	ctx.world = world.get();
	ns::LoopbackChannel wire;
	ctx.np_protocol.connection_list.push_back(conn_fixture::make_conn(
			20, inmatch::NapiNPConnection::kTypeClientSide, &wire, ns::TransportMode::Client,
			w::EntityHandle{}, false));
	inmatch::NapiNPConnection &conn = ctx.np_protocol.connection_list.front();
	conn.admission_stage = stage;
	conn.burst.game_state = game_state;
	conn.link.round_watermark = watermark;
	inmatch::ServerDispatchInputs inputs;
	inputs.server_ctx = &ctx;
	const io::CrtRand before = world->crt_rand;
	AdmissionRun run;
	run.replies = inmatch::dispatch_session_replies(ctx.config, conn, records, 7,
			ctx.np_protocol.connection_list, world.get(), inputs);
	run.stage = conn.admission_stage;
	run.game_state = conn.burst.game_state;
	run.watermark = conn.link.round_watermark;
	io::CrtRand count = before;
	while (count.state != world->crt_rand.state && run.draws < 10000) {
		(void)count.next();
		++run.draws;
	}
	return run;
}

std::vector<uint8_t> game_tags(const std::vector<ProtocolMessage> &replies) {
	std::vector<uint8_t> tags;
	for (const ProtocolMessage &m : replies)
		if (!m.flags.settings_update && m.full_tag < PROTOCOL_FULL_TAG_HIGH_BASE)
			tags.push_back(m.tag);
	return tags;
}

const std::vector<uint8_t> kCompletion{s2c::SYNC_STATE, s2c::PLAYER_NAME, s2c::FULL_PLAYER_INFO,
		s2c::SYNC_TICK};

void test_the_echo_has_no_gate() {
	using S = inmatch::GameAdmissionStage;
	const std::vector<uint8_t> foreign(256, 0xAB);
	// Ahead of the JOIN (game state 1): the admission completes, the validator skipped.
	AdmissionRun run = run_admission(S::AwaitJoinRequest, {make_protocol_message(0x02, foreign)});
	CHECK(game_tags(run.replies) == kCompletion);
	CHECK(run.stage == S::Complete && run.game_state == 7 && run.draws == 0);
	// The probe's state with a foreign echo (no comparison with the probe's words).
	run = run_admission(S::AwaitPaddingEcho, {make_protocol_message(0x02, foreign)});
	CHECK(game_tags(run.replies) == kCompletion && run.stage == S::Complete);
	// The ticket wait and the form post alike.
	run = run_admission(S::AwaitPlayEnter, {make_protocol_message(0x02, {})});
	CHECK(game_tags(run.replies) == kCompletion && run.stage == S::Complete);
	run = run_admission(S::AwaitFormPost, {make_protocol_message(0x02, {})});
	CHECK(game_tags(run.replies) == kCompletion && run.stage == S::Complete);
	// At a completed admission it runs again: the records re-sent, game state 7, and leaving
	// game state 10 disarms the round-event watermark.
	run = run_admission(S::Complete, {make_protocol_message(0x02, foreign)}, 10, 5);
	CHECK(game_tags(run.replies) == kCompletion);
	CHECK(run.stage == S::Complete && run.game_state == 7 && run.watermark == 0);
}

// Game state 7 zeroes the net player's hold-nag stamp (+0x24): a node the spawn pump held and
// nagged a moment ago is nagged again at the next hold after a repeated 0x02.
void test_the_echo_zeroes_the_nag_stamp() {
	auto world = std::make_unique<w::World>(); // heap: 1 MB stack
	inmatch::NapiNPServerCtx ctx;
	ctx.is_authority = 1;
	ctx.is_in_session = 1;
	ctx.world = world.get();
	ns::LoopbackChannel wire;
	ctx.np_protocol.connection_list.push_back(conn_fixture::make_conn(
			20, inmatch::NapiNPConnection::kTypeServerSide, &wire, ns::TransportMode::Client,
			w::EntityHandle{}, false));
	inmatch::NapiNPConnection &conn = ctx.np_protocol.connection_list.front();
	conn.admission_stage = inmatch::GameAdmissionStage::Complete;
	conn.reply.admission_hold_nag_host_ms = 4000;
	conn.reply.admission_hold_nag_host_ms_valid = true;
	inmatch::ServerDispatchInputs inputs;
	inputs.server_ctx = &ctx;
	(void)inmatch::dispatch_session_replies(ctx.config, conn, {make_protocol_message(0x02, {})}, 7,
			ctx.np_protocol.connection_list, world.get(), inputs);
	CHECK(!conn.reply.admission_hold_nag_host_ms_valid);
}

void test_each_admission_record_reaches_its_handler() {
	using S = inmatch::GameAdmissionStage;
	// JOIN then form post in one datagram: the INIT, then the probe (500 draws).
	AdmissionRun run = run_admission(S::AwaitJoinRequest,
			{make_protocol_message(0x00, {}), make_protocol_message(0x01, {0x00})});
	CHECK(game_tags(run.replies) == (std::vector<uint8_t>{s2c::INIT, s2c::JOIN_PADDING_PROBE}));
	CHECK(run.stage == S::AwaitPaddingEcho && run.draws == 500);
	// JOIN, form post and echo: the INIT, the probe and the completion's records.
	run = run_admission(S::AwaitJoinRequest,
			{make_protocol_message(0x00, {}), make_protocol_message(0x01, {0x00}),
					make_protocol_message(0x02, std::vector<uint8_t>(256, 0))});
	std::vector<uint8_t> want{s2c::INIT, s2c::JOIN_PADDING_PROBE};
	want.insert(want.end(), kCompletion.begin(), kCompletion.end());
	CHECK(game_tags(run.replies) == want && run.stage == S::Complete);
	// The form post ahead of the JOIN is out of turn: ignored, then the JOIN is answered.
	run = run_admission(S::AwaitJoinRequest,
			{make_protocol_message(0x01, {0x00}), make_protocol_message(0x00, {})});
	CHECK(game_tags(run.replies) == (std::vector<uint8_t>{s2c::INIT}) && run.stage == S::AwaitFormPost);
	// A game record ahead of the admission reaches its handler, whose server-slot gate answers
	// nothing: the node stays in its stage and the walk goes on.
	for (const uint8_t tag : {uint8_t{0x0A}, uint8_t{0x2F}, uint8_t{0x0B}, uint8_t{0x0C}}) {
		run = run_admission(S::AwaitJoinRequest, {make_protocol_message(tag, {0x01, 0x02})});
		CHECK(run.replies.empty() && run.stage == S::AwaitJoinRequest && run.draws == 0);
	}
	// The 0x0A ahead of the JOIN: the JOIN behind it is answered.
	run = run_admission(S::AwaitJoinRequest,
			{make_protocol_message(0x0A, {}), make_protocol_message(0x00, {})});
	CHECK(game_tags(run.replies) == (std::vector<uint8_t>{s2c::INIT}) && run.stage == S::AwaitFormPost);
	// The 0x2F in the probe's state, then the echo: the admission completes.
	run = run_admission(S::AwaitPaddingEcho,
			{make_protocol_message(0x2F, {0xFF}), make_protocol_message(0x02, {})});
	CHECK(game_tags(run.replies) == kCompletion && run.stage == S::Complete);
	// The 0x0B in the form-post state, then the form post: the probe.
	run = run_admission(S::AwaitFormPost,
			{make_protocol_message(0x0B, {}), make_protocol_message(0x01, {0x00})});
	CHECK(game_tags(run.replies) == (std::vector<uint8_t>{s2c::JOIN_PADDING_PROBE}) &&
			run.stage == S::AwaitPaddingEcho);
}

} // namespace

int main() {
	test_the_side_passwords_second_test();
	test_out_of_turn_join_records_answer_nothing();
	test_the_echo_has_no_gate();
	test_each_admission_record_reaches_its_handler();
	test_the_echo_zeroes_the_nag_stamp();
	std::printf("join_form_post_gate: %d failure(s)\n", g_failures);
	return g_failures == 0 ? 0 : 1;
}
