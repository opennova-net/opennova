// The NovaWorld service the menu dials (net/novaworld/service_target.h): the OpenNova target's
// gate for each build and launch, the gate each target dials, the OpenNova target's web site off
// a gate (NovaLogic's hosts asked nothing), and the stored and launch choices read back. Moved from the GUT
// novaworld_panel tests (ADR 0077 R3).
#include <net/novaworld/service_target.h>

#include <cstdio>
#include <string>

using namespace opennova;

static int failures = 0;
#define CHECK(c)                                                                                   \
	do {                                                                                           \
		if (!(c)) {                                                                                \
			std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c);                               \
			++failures;                                                                            \
		}                                                                                          \
	} while (0)

namespace {

// --novaworld-host wins; else a release export dials prod's, a debug export staging's, a source
// run loopback (never a live service).
void test_opennova_gate_host() {
	CHECK(opennova_gate_host("", true, false) == "nw.opennova.net");
	CHECK(opennova_gate_host("", true, true) == "nw.staging.opennova.net");
	CHECK(opennova_gate_host("", false, true) == "127.0.0.1");
	CHECK(opennova_gate_host("", false, false) == "127.0.0.1");
	CHECK(opennova_gate_host("nw.example.test", true, false) == "nw.example.test");
	CHECK(opennova_gate_host("nw.example.test", true, true) == "nw.example.test");
	CHECK(opennova_gate_host("127.0.0.1", false, true) == "127.0.0.1");
	CHECK(target_gate_host(NovaWorldTarget::Original, "127.0.0.1") == "gs.novaworld.net");
	CHECK(target_gate_host(NovaWorldTarget::OpenNova, "nw.example.test") == "nw.example.test");
}

void test_web_base() {
	CHECK(service_web_base("nw.opennova.net") == "https://opennova.net");
	CHECK(service_web_base("nw.staging.opennova.net") == "https://staging.opennova.net");
	CHECK(service_web_base("127.0.0.1") == "http://127.0.0.1:8000");
	CHECK(service_web_base("play.example.org") == "https://play.example.org");
	CHECK(service_web_base("192.168.1.20") == "http://192.168.1.20:8000");
	CHECK(service_web_base("localhost") == "http://localhost:8000");
	CHECK(service_web_base("::1") == "http://[::1]:8000");
	CHECK(service_web_base("fe80::1:2") == "http://[fe80::1:2]:8000");
	// NovaLogic's gate serves no mods, in any case, a root label's dot naming it too (the
	// engine's one domain test, gate_probe.h is_novaworld_domain_host).
	CHECK(service_web_base("gs.novaworld.net").empty());
	CHECK(service_web_base("GS.NovaWorld.net").empty());
	CHECK(service_web_base("gs.novaworld.net.").empty());
	CHECK(service_web_base("novaworld.net").empty());
	CHECK(service_web_base("notnovaworld.net") == "https://notnovaworld.net");
	// A name that only looks like an address is a name.
	CHECK(service_web_base("1.2.3") == "https://1.2.3");
	CHECK(service_web_base("256.1.1.1") == "https://256.1.1.1");
}

// Nothing stored, a value of another kind or out of range is OpenNova's; a launch naming a gate
// opens on OpenNova's unless it is a mod install's join relaunch.
void test_stored_and_launch_targets() {
	CHECK(stored_novaworld_target(std::nullopt) == NovaWorldTarget::OpenNova);
	CHECK(stored_novaworld_target(0) == NovaWorldTarget::OpenNova);
	CHECK(stored_novaworld_target(1) == NovaWorldTarget::Original);
	CHECK(stored_novaworld_target(2) == NovaWorldTarget::OpenNova);
	CHECK(stored_novaworld_target(-1) == NovaWorldTarget::OpenNova);
	CHECK(launch_novaworld_target("", false, NovaWorldTarget::Original) ==
			NovaWorldTarget::Original);
	CHECK(launch_novaworld_target("127.0.0.1", false, NovaWorldTarget::Original) ==
			NovaWorldTarget::OpenNova);
	CHECK(launch_novaworld_target("127.0.0.1", true, NovaWorldTarget::Original) ==
			NovaWorldTarget::Original);
}

} // namespace

int main() {
	test_opennova_gate_host();
	test_web_base();
	test_stored_and_launch_targets();
	if (failures == 0)
		std::printf("novaworld_service_target: ok\n");
	return failures == 0 ? 0 : 1;
}
