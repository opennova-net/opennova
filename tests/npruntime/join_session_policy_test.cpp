// Unit tests for inmatch::JoinSessionPolicy + decide_join_expansion: the joiner
// session-drive policy the shell's net-session drive executes: the preload's
// failure edge, the S2C 0x7B promote validation, the admission/deploy/loss edge
// machine, and the D-NET-178 expansion reconcile decision. This locks the
// admission-frame ORDERING (loss > settle > join error > edges) and the edge
// latches. No wait has a deadline, as retail's join screen and mission-start waits
// have none (D-NET-NEW-L2-44). The watch takes no ESC (D-NET-NEW-L2-39): the hold
// after the S2C 0x0F is the game loop's.

#include <runtime/inmatch/join_session_policy.h>

#include <cstdio>
#include <string>
#include <vector>
#include "../common/test_expect.h"

namespace {

namespace inmatch = opennova::inmatch;

int &failures = test_expect::failures;

using test_expect::expect;

bool contains(const std::string &s, const char *needle) {
	return s.find(needle) != std::string::npos;
}

// -- decide_join_expansion ---------------------------------------------------

void test_expansion_decision() {
	{
		const auto plan = inmatch::decide_join_expansion("jox01", "jox01", { "jox01" });
		expect(plan.action == inmatch::JoinExpansionAction::kKeep, "matching expansion is left alone");
		expect(plan.error.empty(), "keep carries no error");
	}
	{
		const auto plan = inmatch::decide_join_expansion("", "", { "jox01" });
		expect(plan.action == inmatch::JoinExpansionAction::kKeep,
				"a base-game host and a base-game mount already agree");
	}
	{
		const auto plan = inmatch::decide_join_expansion("jox01", "", { "jox01" });
		expect(plan.action == inmatch::JoinExpansionAction::kRemount,
				"expansion host against a base mount remounts");
		expect(plan.expansion == "jox01", "...to the host expansion");
	}
	{
		// The direction that is easy to miss: an expansion mounted locally while
		// the host runs base JO misreads the wire exactly as badly as the
		// reverse (the ADM index space shifts in both directions). Empty is a
		// value here, never "no action".
		const auto plan = inmatch::decide_join_expansion("", "jox01", { "jox01" });
		expect(plan.action == inmatch::JoinExpansionAction::kRemount,
				"base host against an expansion mount remounts to base");
		expect(plan.expansion.empty(), "base game is the target, always mountable");
	}
	{
		const auto plan = inmatch::decide_join_expansion("revx02", "jox01", { "jox01" });
		expect(plan.action == inmatch::JoinExpansionAction::kFail,
				"uninstalled host expansion fails");
		expect(contains(plan.error, "revx02"), "...naming the host expansion");
		expect(contains(plan.error, "jox01"), "...and what is installed");
	}
	{
		const auto plan = inmatch::decide_join_expansion("jox01", "", {});
		expect(plan.action == inmatch::JoinExpansionAction::kFail,
				"uninstalled expansion on a base-only install fails");
		expect(contains(plan.error, "none"), "...saying nothing is installed");
	}
	{
		expect(inmatch::decide_join_expansion("JOX01", "jox01", { "jox01" }).action ==
						inmatch::JoinExpansionAction::kKeep,
				"comparisons are case-insensitive");
		const auto plan = inmatch::decide_join_expansion(" JOX01 ", "", { "jox01" });
		expect(plan.action == inmatch::JoinExpansionAction::kRemount,
				"...and whitespace-insensitive");
		expect(plan.expansion == "jox01",
				"the mount uses the ON-DISK spelling, not the wire's");
	}
}

// -- the pre-load waits --------------------------------------------------------

// The join screen holds in the host's queue until the S2C 0x05, a lost session or
// Cancel, and the mission start's waits until their record, the disconnect or ESC: no
// deadline. Every pass of a held join waits; only a join error fails it.
// [orig: MultiPlayer_JoinSessionStateMachine @0x56A320 - the session tests @0x56A5E7 /
//  @0x56A6DF / @0x56A758, the abort @0x56A59C / @0x56A67F, its clock read only by the
//  gate ping @0x56A4E8 and the queue line @0x56A7A2; NapiClient_WaitForGameStart
//  @0x42CC10 - GetTickCount @0x42CC4A / @0x42CC5D discarded] (D-NET-NEW-L2-44)
void test_preload_waits_carry_no_deadline() {
	inmatch::JoinSessionPolicy p;
	// The p195 tdh-sidepw RO hold: the retail host nagged with its S2C 0x03 for 60 s;
	// whatever the passes, none fails without a join error.
	bool waited = true;
	for (int pass = 0; pass < 100000; ++pass)
		waited = waited && p.preload_step("") == inmatch::JoinPreloadStep::kWait;
	expect(waited, "a held join waits on every pass");
	expect(p.fail_reason().empty(), "...and composes no failure");
	expect(p.preload_step("no route to host") == inmatch::JoinPreloadStep::kFail,
			"a join error fails");
	expect(p.fail_reason() == "join failed: no route to host",
			"...with the error composed");
}

// -- the S2C 0x7B promote validation -----------------------------------------

void test_promote_validation() {
	inmatch::JoinSessionPolicy p;
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
	expect(p.validate_promote_header(inmatch::kJoinWireMissionHeaderBytes),
			"the exact S2C 0x0B header size passes");
}

// -- the admission watch + edge machine ---------------------------------------

void test_admission_watch() {
	inmatch::JoinSessionPolicy p;
	p.arm_admission_watch();
	expect(p.admission_watch_active(), "arm_admission_watch arms");

	// The server still owes the admission: every frame waits, with no deadline
	// (D-NET-NEW-L2-44).
	bool waited = true;
	for (int frame = 0; frame < 100000; ++frame) {
		waited = waited && p.begin_admission_frame("", false, false, "") == 0 &&
				p.finish_admission_frame(true, false) == 0;
	}
	expect(waited, "a frame the server still owes requests and emits nothing, however many");
	expect(p.admission_watch_active(), "...with the watch still armed");

	// A join error fails the watch.
	p.begin_admission_frame("", false, false, "kicked");
	const uint32_t post = p.finish_admission_frame(true, false);
	expect(post == inmatch::kAdmissionLoadFailed, "a join error fails the watch");
	expect(p.fail_reason() == "join failed: kicked", "...with the error text");
	expect(!p.admission_watch_active(), "...and disarms");
}

void test_admission_settle_gate() {
	inmatch::JoinSessionPolicy p;
	p.arm_admission_watch();
	uint32_t pre = p.begin_admission_frame("", true, false, "");
	expect((pre & inmatch::kAdmissionSettleRequired) != 0,
			"a pending deploy pick requires the settle");
	uint32_t post = p.finish_admission_frame(false, false);
	expect(post == inmatch::kAdmissionSettleFailed, "a failed settle aborts");
	expect(contains(p.fail_reason(), "settle"), "...through the wire-asset leg");
	expect(!p.admission_watch_active(), "...and disarms");

	p.arm_admission_watch();
	pre = p.begin_admission_frame("", false, true, "");
	expect((pre & inmatch::kAdmissionSettleRequired) != 0,
			"initial admission also requires the settle");
}

void test_admission_deploy_edge_and_ready() {
	inmatch::JoinSessionPolicy p;
	p.arm_admission_watch();

	// Deploy pick pending: settle, disarm immediately (the DEATH screen is the
	// hold), rising edge emitted once.
	p.begin_admission_frame("", true, false, "");
	uint32_t post = p.finish_admission_frame(true, false);
	expect((post & inmatch::kAdmissionEmitDeployPick) != 0, "the deploy edge emits");
	expect(!p.admission_watch_active(), "a deploy pick disarms immediately");
	p.begin_admission_frame("", true, false, "");
	post = p.finish_admission_frame(true, false);
	expect(post == 0, "a held pick does not re-emit");

	// The pick releases with the world admitted and drained: ready emits once,
	// with the deploy latch re-armed for a later death.
	p.begin_admission_frame("", false, true, "");
	post = p.finish_admission_frame(true, true);
	expect((post & inmatch::kAdmissionEmitReady) != 0,
			"admission-ready emits after the pick releases");
	p.begin_admission_frame("", false, true, "");
	post = p.finish_admission_frame(true, true);
	expect(post == 0, "admission-ready is once per join");

	// Death later in the session re-opens DEATH through the same edge.
	p.begin_admission_frame("", true, true, "");
	post = p.finish_admission_frame(true, true);
	expect((post & inmatch::kAdmissionEmitDeployPick) != 0,
			"a later death re-emits the deploy edge after the release");
}

void test_admission_ready_holds_for_wire_drain() {
	inmatch::JoinSessionPolicy p;
	p.arm_admission_watch();
	// Initial admission complete but the cold wire drain has not emptied: the
	// reveal holds WITH the watch still armed.
	p.begin_admission_frame("", false, true, "");
	uint32_t post = p.finish_admission_frame(true, false);
	expect(post == 0, "the no-pick reveal holds behind the cold wire drain");
	expect(p.admission_watch_active(), "...with the watch still armed");
	// Drained: disarm + ready.
	p.begin_admission_frame("", false, true, "");
	post = p.finish_admission_frame(true, true);
	expect((post & inmatch::kAdmissionEmitReady) != 0, "the drained reveal admits");
	expect(!p.admission_watch_active(), "...and disarms");
}

void test_session_loss_latch() {
	inmatch::JoinSessionPolicy p;
	p.arm_admission_watch();
	uint32_t pre = p.begin_admission_frame("host punt", true, true, "");
	expect(pre == (inmatch::kAdmissionFrameDone | inmatch::kAdmissionEmitSessionLost),
			"terminal state wins over every admission edge");
	expect(p.session_loss_reason() == "host punt", "...carrying the reason");
	expect(!p.admission_watch_active(), "...and disarms");
	pre = p.begin_admission_frame("host punt", true, true, "");
	expect(pre == inmatch::kAdmissionFrameDone,
			"one session-loss notification per session; later frames only suppress");
	// A fresh join attempt re-arms the notification.
	p.reset_for_join();
	pre = p.begin_admission_frame("host punt", false, false, "");
	expect((pre & inmatch::kAdmissionEmitSessionLost) != 0,
			"reset_for_join re-arms the loss notification");
}

void test_reset() {
	inmatch::JoinSessionPolicy p;
	p.arm_admission_watch();
	p.begin_admission_frame("gone", false, false, "");
	p.reset();
	expect(!p.admission_watch_active(), "reset disarms the admission watch");
	const uint32_t pre = p.begin_admission_frame("gone", false, false, "");
	expect((pre & inmatch::kAdmissionEmitSessionLost) != 0,
			"reset clears the session-loss latch");
}

} // namespace

int main() {
	test_expansion_decision();
	test_preload_waits_carry_no_deadline();
	test_promote_validation();
	test_admission_watch();
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
