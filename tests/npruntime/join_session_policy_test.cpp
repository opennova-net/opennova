// Unit tests for np::JoinSessionPolicy + decide_join_expansion — the joiner
// session-drive policy the shell's net-session drive executes: the two
// ConnectOrHost windows (0xEA60 reachable-analog reuse), the S2C 0x7B promote
// validation, the admission/deploy/loss edge machine, and the D-NET-178
// expansion reconcile decision. This locks the admission-frame ORDERING
// (loss > abort > settle > window > edges) and the edge latches.

#include <runtime/session/join_session_policy.h>

#include <cstdio>
#include <string>
#include <vector>

namespace {

namespace np = opennova::np;

int failures = 0;

bool expect(bool cond, const char *msg) {
	if (cond) return true;
	std::fprintf(stderr, "FAIL: %s\n", msg);
	++failures;
	return false;
}

bool contains(const std::string &s, const char *needle) {
	return s.find(needle) != std::string::npos;
}

// -- decide_join_expansion ---------------------------------------------------

void test_expansion_decision() {
	{
		const auto plan = np::decide_join_expansion("jox01", "jox01", { "jox01" });
		expect(plan.action == np::JoinExpansionAction::kKeep, "matching expansion is left alone");
		expect(plan.error.empty(), "keep carries no error");
	}
	{
		const auto plan = np::decide_join_expansion("", "", { "jox01" });
		expect(plan.action == np::JoinExpansionAction::kKeep,
				"a base-game host and a base-game mount already agree");
	}
	{
		const auto plan = np::decide_join_expansion("jox01", "", { "jox01" });
		expect(plan.action == np::JoinExpansionAction::kRemount,
				"expansion host against a base mount remounts");
		expect(plan.expansion == "jox01", "...to the host expansion");
	}
	{
		// The direction that is easy to miss: an expansion mounted locally while
		// the host runs base JO misreads the wire exactly as badly as the
		// reverse (the ADM index space shifts in both directions). Empty is a
		// value here, never "no action".
		const auto plan = np::decide_join_expansion("", "jox01", { "jox01" });
		expect(plan.action == np::JoinExpansionAction::kRemount,
				"base host against an expansion mount remounts to base");
		expect(plan.expansion.empty(), "base game is the target, always mountable");
	}
	{
		const auto plan = np::decide_join_expansion("revx02", "jox01", { "jox01" });
		expect(plan.action == np::JoinExpansionAction::kFail,
				"uninstalled host expansion fails");
		expect(contains(plan.error, "revx02"), "...naming the host expansion");
		expect(contains(plan.error, "jox01"), "...and what is installed");
	}
	{
		const auto plan = np::decide_join_expansion("jox01", "", {});
		expect(plan.action == np::JoinExpansionAction::kFail,
				"uninstalled expansion on a base-only install fails");
		expect(contains(plan.error, "none"), "...saying nothing is installed");
	}
	{
		expect(np::decide_join_expansion("JOX01", "jox01", { "jox01" }).action ==
						np::JoinExpansionAction::kKeep,
				"comparisons are case-insensitive");
		const auto plan = np::decide_join_expansion(" JOX01 ", "", { "jox01" });
		expect(plan.action == np::JoinExpansionAction::kRemount,
				"...and whitespace-insensitive");
		expect(plan.expansion == "jox01",
				"the mount uses the ON-DISK spelling, not the wire's");
	}
}

// -- the pre-load connect/session window -------------------------------------

void test_preload_window() {
	np::JoinSessionPolicy p;
	p.arm_preload(1000);
	expect(p.preload_armed(), "arm_preload arms");
	expect(p.preload_step("", 1000) == np::JoinPreloadStep::kWait,
			"fresh window waits");
	expect(p.preload_step("", 1000 + np::kJoinConnectWindowMs - 1) ==
					np::JoinPreloadStep::kWait,
			"one ms before the window closes it still waits");
	expect(p.preload_step("", 1000 + np::kJoinConnectWindowMs) ==
					np::JoinPreloadStep::kFail,
			"the retail ConnectOrHost window (0xEA60) closes the preload");
	expect(contains(p.fail_reason(), "timed out"), "...as a timeout");
	p.arm_preload(1000);
	expect(p.preload_step("no route to host", 999999999) == np::JoinPreloadStep::kFail,
			"a join error fails");
	expect(p.fail_reason() == "join failed: no route to host",
			"...with the error composed first, ahead of the window");
	p.disarm_preload();
	expect(!p.preload_armed(), "disarm_preload disarms");
	expect(p.preload_step("", 999999999) == np::JoinPreloadStep::kWait,
			"a disarmed window never times out");
}

// -- the S2C 0x7B promote validation -----------------------------------------

void test_promote_validation() {
	np::JoinSessionPolicy p;
	expect(!p.validate_promote_mission_file("   "), "an empty map_file fails");
	expect(contains(p.fail_reason(), "empty map_file"), "...saying so");
	expect(p.validate_promote_mission_file(" AShi5A "), "a bare basename passes");
	expect(p.promoted_mission_file() == "AShi5A.bms",
			"...trimmed, with .bms appended");
	expect(p.validate_promote_mission_file("mnml.BMS"), "an extension passes");
	expect(p.promoted_mission_file() == "mnml.BMS",
			"...matched case-insensitively and kept verbatim");
	expect(!p.validate_promote_header(615), "a short header fails");
	expect(contains(p.fail_reason(), "616-byte"), "...naming the exact size");
	expect(!p.validate_promote_header(0), "a missing header fails");
	expect(p.validate_promote_header(np::kJoinWireMissionHeaderBytes),
			"the exact S2C 0x0B header size passes");
}

// -- the admission watchdog + edge machine -----------------------------------

void test_admission_watchdog_window() {
	np::JoinSessionPolicy p;
	p.arm_admission_watch(5000);
	expect(p.admission_watch_active(), "arm_admission_watch arms");

	// Nothing owed yet, window open: no edges, no settle needed.
	uint32_t pre = p.begin_admission_frame("", false, false, "", "HELLO", 5000);
	expect(pre == 0, "an idle open-window frame requests nothing");
	expect(p.finish_admission_frame(true, false) == 0, "...and emits nothing");

	// The window closes with the server still owing the admission: stage-named
	// failure.
	pre = p.begin_admission_frame("", false, false, "", "WORLD_STREAM",
			5000 + np::kJoinConnectWindowMs);
	expect(pre == 0, "the deadline is evaluated in the finish half");
	uint32_t post = p.finish_admission_frame(true, false);
	expect(post == np::kAdmissionLoadFailed, "the closed window fails the join");
	expect(contains(p.fail_reason(), "stalled"), "...as a stall");
	expect(contains(p.fail_reason(), "WORLD_STREAM"), "...naming the admission stage");
	expect(!p.admission_watch_active(), "...and disarms");

	// A join error beats the window.
	p.arm_admission_watch(5000);
	p.begin_admission_frame("", false, false, "kicked", "HELLO", 5000);
	post = p.finish_admission_frame(true, false);
	expect(post == np::kAdmissionLoadFailed, "a join error fails the watch");
	expect(p.fail_reason() == "join failed: kicked", "...with the error text");
}

void test_admission_abort() {
	np::JoinSessionPolicy p;
	expect(!p.request_admission_abort(), "no live watch: nothing to abort");
	p.arm_admission_watch(0);
	expect(p.request_admission_abort(), "a live watch accepts the abort");
	const uint32_t pre = p.begin_admission_frame("", false, false, "", "HELLO", 1);
	expect(pre == (np::kAdmissionFrameDone | np::kAdmissionLoadFailed),
			"the abort fails the frame terminally");
	expect(p.fail_reason() == "Mission loading aborted",
			"...with the retail ESC-abort wording [orig: @0x520270]");
	expect(!p.admission_watch_active(), "...and disarms");
}

void test_admission_settle_gate() {
	np::JoinSessionPolicy p;
	p.arm_admission_watch(0);
	uint32_t pre = p.begin_admission_frame("", true, false, "", "", 1);
	expect((pre & np::kAdmissionSettleRequired) != 0,
			"a pending deploy pick requires the settle");
	uint32_t post = p.finish_admission_frame(false, false);
	expect(post == np::kAdmissionSettleFailed, "a failed settle aborts");
	expect(contains(p.fail_reason(), "settle"), "...through the wire-asset leg");
	expect(!p.admission_watch_active(), "...and disarms");

	p.arm_admission_watch(0);
	pre = p.begin_admission_frame("", false, true, "", "", 1);
	expect((pre & np::kAdmissionSettleRequired) != 0,
			"initial admission also requires the settle");
}

void test_admission_deploy_edge_and_ready() {
	np::JoinSessionPolicy p;
	p.arm_admission_watch(0);

	// Deploy pick pending: settle, disarm immediately (the DEATH screen is the
	// hold), rising edge emitted once.
	p.begin_admission_frame("", true, false, "", "", 1);
	uint32_t post = p.finish_admission_frame(true, false);
	expect((post & np::kAdmissionEmitDeployPick) != 0, "the deploy edge emits");
	expect(!p.admission_watch_active(), "a deploy pick disarms immediately");
	p.begin_admission_frame("", true, false, "", "", 2);
	post = p.finish_admission_frame(true, false);
	expect(post == 0, "a held pick does not re-emit");

	// The pick releases with the world admitted and drained: ready emits once,
	// with the deploy latch re-armed for a later death.
	p.begin_admission_frame("", false, true, "", "", 3);
	post = p.finish_admission_frame(true, true);
	expect((post & np::kAdmissionEmitReady) != 0,
			"admission-ready emits after the pick releases");
	p.begin_admission_frame("", false, true, "", "", 4);
	post = p.finish_admission_frame(true, true);
	expect(post == 0, "admission-ready is once per join");

	// Death later in the session re-opens DEATH through the same edge.
	p.begin_admission_frame("", true, true, "", "", 5);
	post = p.finish_admission_frame(true, true);
	expect((post & np::kAdmissionEmitDeployPick) != 0,
			"a later death re-emits the deploy edge after the release");
}

void test_admission_ready_holds_for_wire_drain() {
	np::JoinSessionPolicy p;
	p.arm_admission_watch(100);
	// Initial admission complete but the cold wire drain has not emptied: the
	// reveal holds WITH the watchdog deadline still armed.
	p.begin_admission_frame("", false, true, "", "SPAWNED", 101);
	uint32_t post = p.finish_admission_frame(true, false);
	expect(post == 0, "the no-pick reveal holds behind the cold wire drain");
	expect(p.admission_watch_active(), "...with the watchdog still armed");
	// A drain that never empties eventually hits the window.
	p.begin_admission_frame("", false, true, "", "SPAWNED",
			101 + np::kJoinConnectWindowMs);
	post = p.finish_admission_frame(true, false);
	expect(post == np::kAdmissionLoadFailed, "a stalled drain hits the window");
	// Drained: disarm + ready.
	p.arm_admission_watch(100);
	p.begin_admission_frame("", false, true, "", "", 101);
	post = p.finish_admission_frame(true, true);
	expect((post & np::kAdmissionEmitReady) != 0, "the drained reveal admits");
	expect(!p.admission_watch_active(), "...and disarms");
}

void test_session_loss_latch() {
	np::JoinSessionPolicy p;
	p.arm_admission_watch(0);
	uint32_t pre = p.begin_admission_frame("host punt", true, true, "", "", 1);
	expect(pre == (np::kAdmissionFrameDone | np::kAdmissionEmitSessionLost),
			"terminal state wins over every admission edge");
	expect(p.session_loss_reason() == "host punt", "...carrying the reason");
	expect(!p.admission_watch_active(), "...and disarms");
	pre = p.begin_admission_frame("host punt", true, true, "", "", 2);
	expect(pre == np::kAdmissionFrameDone,
			"one session-loss notification per session; later frames only suppress");
	// A fresh join attempt re-arms the notification.
	p.reset_for_join();
	pre = p.begin_admission_frame("host punt", false, false, "", "", 3);
	expect((pre & np::kAdmissionEmitSessionLost) != 0,
			"reset_for_join re-arms the loss notification");
}

void test_reset() {
	np::JoinSessionPolicy p;
	p.arm_preload(1);
	p.arm_admission_watch(1);
	p.begin_admission_frame("gone", false, false, "", "", 2);
	p.reset();
	expect(!p.preload_armed(), "reset disarms the preload window");
	expect(!p.admission_watch_active(), "reset disarms the admission watch");
	const uint32_t pre = p.begin_admission_frame("gone", false, false, "", "", 3);
	expect((pre & np::kAdmissionEmitSessionLost) != 0,
			"reset clears the session-loss latch");
}

} // namespace

int main() {
	test_expansion_decision();
	test_preload_window();
	test_promote_validation();
	test_admission_watchdog_window();
	test_admission_abort();
	test_admission_settle_gate();
	test_admission_deploy_edge_and_ready();
	test_admission_ready_holds_for_wire_drain();
	test_session_loss_latch();
	test_reset();
	if (failures) {
		std::fprintf(stderr, "%d failure(s)\n", failures);
		return 1;
	}
	std::printf("join_session_policy: all checks passed\n");
	return 0;
}
