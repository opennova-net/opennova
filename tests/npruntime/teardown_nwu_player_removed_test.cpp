// D-NET-NEW-L9-29: a player's teardown on the host runs the process's NovaWorld session's leg first
// (ClientHostPlayerRemoved in state 6 and the slot's PlayerList vars dropped), through the host
// context's NapiNPServerCtx::nwu_player_removed: a remote's teardown and the listen host's own at
// its leave alike, for a slot whose player was added, and nothing for a connection that never
// reached its player add. The LN S1 retail host record (2026-10-23) shows the own one at the
// leave: ClientHostPlayerRemoved (46 B), then ClientStopHosting 14 ms later.
// [orig: Server_HandlePlayerDisconnect @0x51B5C0 - the authority test @0x51B5D0, the slot's
//  active byte @0x51B5DC, CNapiGameSession_SendPlayerRemovedIfHosting called @0x51B5F3,
//  NapiLinkedList_RemoveByUserData called @0x51B601, ahead of the +5 test @0x51B606]
#include <runtime/inmatch/napi_np_connection.h>
#include <runtime/inmatch/napi_np_protocol.h>
#include <runtime/inmatch/napi_np_server_ctx.h>

#include <cstdio>
#include <functional>
#include <memory>
#include <vector>

namespace {

using namespace opennova;

int failures = 0;
#define CHECK(c)                                                                     \
	do {                                                                             \
		if (!(c)) {                                                                  \
			std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c);                 \
			++failures;                                                              \
		}                                                                            \
	} while (0)

inmatch::NapiNPConnection remote(uint16_t port, uint8_t slot, inmatch::ConnectionPhase phase) {
	inmatch::NapiNPConnection c;
	c.type = inmatch::NapiNPConnection::kTypeServerSide;
	c.peer = PeerAddr{0x0100007Fu, port};
	c.phase = phase;
	c.reply.player_slot = slot;
	return c;
}

// A remote's teardown with its player added: the leg once, with its slot; one never added: none.
void test_a_remote_teardown_runs_the_leg() {
	auto ctx = std::make_unique<inmatch::NapiNPServerCtx>();
	ctx->is_authority = 1;
	std::vector<int> removed;
	const std::function<void(int)> leg = [&removed](int player_number) {
		removed.push_back(player_number);
	};
	ctx->nwu_player_removed = &leg;
	ctx->np_protocol.connection_list.push_back(remote(4001, 3, inmatch::ConnectionPhase::InMatch));
	ctx->np_protocol.connection_list.push_back(remote(4002, 4, inmatch::ConnectionPhase::Joined));
	CHECK(inmatch::destroy_connection(*ctx, PeerAddr{0x0100007Fu, 4001}, nullptr));
	CHECK(removed == std::vector<int>{3});
	CHECK(inmatch::destroy_connection(*ctx, PeerAddr{0x0100007Fu, 4002}, nullptr));
	CHECK(removed == std::vector<int>{3});
	CHECK(ctx->np_protocol.connection_list.empty());
}

// The listen host's own pair at its leave: the pump that drains its goodbye tears the twin down,
// whose removal runs the leg for the own slot.
void test_the_own_leave_runs_the_leg() {
	auto ctx = std::make_unique<inmatch::NapiNPServerCtx>();
	ctx->is_authority = 1;
	std::vector<int> removed;
	const std::function<void(int)> leg = [&removed](int player_number) {
		removed.push_back(player_number);
	};
	ctx->nwu_player_removed = &leg;
	inmatch::NapiNPConnection own;
	own.type = inmatch::NapiNPConnection::kTypeClientSide;
	own.phase = inmatch::ConnectionPhase::InMatch;
	own.reply.player_slot = 0;
	own.own_goodbye = inmatch::OwnGoodbye::TeardownPump;
	ctx->np_protocol.connection_list.push_back(own);
	ctx->np_protocol.connection_list.push_back(remote(4003, 1, inmatch::ConnectionPhase::InMatch));
	CHECK(inmatch::drain_own_goodbye(*ctx, inmatch::OwnGoodbye::TeardownPump));
	CHECK(removed == std::vector<int>{0});
	CHECK(ctx->np_protocol.connection_list.size() == 1);
}

// No NovaWorld session held: the teardown runs as before, whether the context has no pointer or
// the process's function is unset.
void test_no_session_held() {
	auto ctx = std::make_unique<inmatch::NapiNPServerCtx>();
	ctx->is_authority = 1;
	ctx->np_protocol.connection_list.push_back(remote(4004, 2, inmatch::ConnectionPhase::InMatch));
	CHECK(inmatch::destroy_connection(*ctx, PeerAddr{0x0100007Fu, 4004}, nullptr));
	const std::function<void(int)> unset;
	ctx->nwu_player_removed = &unset;
	ctx->np_protocol.connection_list.push_back(remote(4005, 2, inmatch::ConnectionPhase::InMatch));
	CHECK(inmatch::destroy_connection(*ctx, PeerAddr{0x0100007Fu, 4005}, nullptr));
	CHECK(ctx->np_protocol.connection_list.empty());
}

} // namespace

int main() {
	test_a_remote_teardown_runs_the_leg();
	test_the_own_leave_runs_the_leg();
	test_no_session_held();
	if (failures != 0) {
		std::printf("npruntime_teardown_nwu_player_removed: %d failure(s)\n", failures);
		return 1;
	}
	std::printf("npruntime_teardown_nwu_player_removed: ok\n");
	return 0;
}
