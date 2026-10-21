// D-NET-NEW-L1-21: the host's C2S 0x01 handler. Retail answers a 0x01 only for a net player in
// game state 2 (the JOIN accepted) with no server slot; there it runs the side passwords' second
// test, whatever the team-choose rule the 0x00's validator follows: with either password set, a
// joiner that is no spectator (JSR 0) and whose JSP equals neither password, case-insensitively,
// is rejected with reason 17 and gets no probe (an empty JSP equals an unset password). A 0x01
// after the admission completed (state 6 on) answers nothing and draws nothing from the CRT
// stream.
// [orig: NapiNPServerMsg_HandleJoinRequest @0x512ED0 - `cmp [edi+0A0h], 2` @0x512EF1,
//  `cmp [edi+0C0h], 0` @0x512EFE, the two lengths @0x512F0B..0x512F3B, the JSR test @0x512F3D,
//  _stricmp(JSP, A) @0x512F52 and (JSP, B) @0x512F64, CNapiNPConnection_QueueDisconnectEvent
//  (player, "", 17, "") @0x512F7E; state 6 @0x512FB3 and the probe's call @0x512FBC;
//  NapiNPServer_HandlePlayerJoinMessage @0x512AA0 (state 2 @0x512E7D); Server_ValidatePlayerJoinRequest
//  @0x512100 (its side-password leg only under BuildFlags & 4 @0x5124A2)]

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
			conn, {make_protocol_message(0x01, {0x00})}, 7, ctx.np_protocol.connection_list,
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
	run_form_post({"Alpha", "", "", 0, false});        // an empty JSP against a set A: 17
	run_form_post({"", "Bravo", "", 0, true});         // an empty JSP equals the unset A
	run_form_post({"", "Bravo", "wrong", 0, false});   // a JSP equals neither the unset A nor B
	run_form_post({"Alpha", "Bravo", "wrong", 1, true}); // a spectator request skips the test
}

// A completed admission (state 6 on): a repeated 0x01 answers nothing and draws nothing.
void test_a_repeated_form_post_answers_nothing() {
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
	conn.admission_stage = inmatch::GameAdmissionStage::Complete;
	inmatch::ServerDispatchInputs inputs;
	inputs.server_ctx = &ctx;
	const uint32_t before = world->crt_rand.state;
	const std::vector<ProtocolMessage> replies = inmatch::dispatch_session_replies(ctx.config,
			conn, {make_protocol_message(0x01, {0x00})}, 7, ctx.np_protocol.connection_list,
			world.get(), inputs);
	CHECK(replies.empty());
	CHECK(world->crt_rand.state == before);
	CHECK(conn.admission_stage == inmatch::GameAdmissionStage::Complete);
	ns::Datagram d;
	CHECK(!wire.client_recv(d));
}

} // namespace

int main() {
	test_the_side_passwords_second_test();
	test_a_repeated_form_post_answers_nothing();
	std::printf("join_form_post_gate: %d failure(s)\n", g_failures);
	return g_failures == 0 ? 0 : 1;
}
