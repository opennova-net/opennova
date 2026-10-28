// The session's update legs (inmatch::Session::set_update_head / set_update_tail): the
// embedder's legs run once per main-loop update, the head ahead of the update's tick and the
// tail past it, as retail's update callback runs the NovaWorld session's periodic update ahead
// of its server tick and the remote admin's pump past its exit router; an update whose tick
// ended the session runs no tail, and the transport's forced step runs neither.
// [orig: Game_ProcessMainFrame @0x5263F0 - CNapiGameSession_ProcessPeriodicUpdate @0x526532,
//  Server_TickUpdate @0x5266B6, the exit router @0x526806..0x526867, CAdminServer_ProcessFrame
//  @0x5268F1; Game_MainLoop @0x52BA5A (one call per update)]
#include <runtime/inmatch/session.h>
#include <runtime/mission/mission_kernel.h>

#include <cstdio>
#include <string>

namespace {

using namespace opennova;
using namespace opennova::inmatch;

int failures = 0;
#define CHECK(c)                                                                     \
	do {                                                                             \
		if (!(c)) {                                                                  \
			std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c);                 \
			++failures;                                                              \
		}                                                                            \
	} while (0)

// A host role that writes its ticks into the shared log and can end the session.
struct LoggingRole final : Role {
	mission::MissionKernel kernel;
	std::string *log = nullptr;
	int lose_at_tick = 0; // the tick that reports the session lost (0: none)
	int ticks = 0;
	RoleKind role_kind = RoleKind::DedicatedHost;
	LoggingRole() { bind(kernel); }
	RoleKind kind() const override { return role_kind; }
	void run_tick(const TickInput &) override {
		++ticks;
		++kernel.world.logic_tick;
		*log += 'T';
	}
	bool session_lost(SessionError &error) const override {
		if (lose_at_tick == 0 || ticks < lose_at_tick) return false;
		error = {SessionErrorCode::SessionLost, "mission exit"};
		return true;
	}
	void close() override {}
};

void install(Session &session, std::string &log) {
	session.set_update_head([&log] { log += 'H'; });
	session.set_update_tail([&log] { log += 'A'; });
}

void each_update_runs_head_tick_tail() {
	std::string log;
	LoggingRole role;
	role.log = &log;
	Session session(role);
	install(session, log);
	CHECK(session.begin_load().applied() && session.complete_load().applied());
	// 48 ms banked over the fresh bank's 1 ms: three updates, each its head, tick and tail.
	FrameInput input;
	input.delta_seconds = 0.048;
	const FrameOutcome outcome = session.advance(input);
	CHECK(outcome.ticks_run() == 3);
	CHECK(log == "HTAHTAHTA");
	std::printf("  each update: the head, the tick, the tail\n");
}

void a_frame_with_no_update_runs_no_leg() {
	// A fresh bank's 1 ms with nothing more banked drains no quantum: no update, no leg.
	std::string log;
	LoggingRole role;
	role.log = &log;
	Session session(role);
	install(session, log);
	CHECK(session.begin_load().applied() && session.complete_load().applied());
	FrameInput none;
	none.delta_seconds = 0.0;
	const FrameOutcome outcome = session.advance(none);
	CHECK(outcome.ticks_run() == 0);
	CHECK(log.empty());
	std::printf("  a frame that banks no update runs no leg\n");
}

void an_update_that_ends_the_session_runs_no_tail() {
	std::string log;
	LoggingRole role;
	role.log = &log;
	role.lose_at_tick = 2;
	Session session(role);
	install(session, log);
	CHECK(session.begin_load().applied() && session.complete_load().applied());
	FrameInput input;
	input.delta_seconds = 0.048;
	const FrameOutcome outcome = session.advance(input);
	CHECK(outcome.terminal());
	CHECK(log == "HTAHT");
	std::printf("  the update whose tick ends the session runs its head, no tail\n");
}

void the_transport_step_runs_no_leg() {
	std::string log;
	LoggingRole role;
	role.log = &log;
	role.role_kind = RoleKind::SinglePlayer;
	Session session(role);
	install(session, log);
	CHECK(session.begin_load().applied() && session.complete_load().applied());
	CHECK(session.pause().applied());
	const FrameOutcome outcome = session.step_once(FrameInput{});
	CHECK(outcome.ticks_run() == 1);
	CHECK(log == "T");
	std::printf("  the transport's forced step is no main-loop update: no leg\n");
}

} // namespace

int main() {
	std::printf("session_update_legs:\n");
	each_update_runs_head_tick_tail();
	a_frame_with_no_update_runs_no_leg();
	an_update_that_ends_the_session_runs_no_tail();
	the_transport_step_runs_no_leg();
	if (failures != 0) {
		std::printf("session_update_legs: %d failure(s)\n", failures);
		return 1;
	}
	std::printf("session_update_legs: OK\n");
	return 0;
}
