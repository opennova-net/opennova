// The NovaWorld destination policies (apps/nw_lister/policy.h): opennova-nw-lister's
// loopback-only until --allow-public, and opennova-serve's (ADR 0051 d6): loopback and any host
// off NovaLogic's domain by default, a name under novaworld.net only with --allow-public
// [orig: CNapiGateManager_Init @0x633f90 (host "novaworld.net"); "gs.novaworld.net" @0x7cc368].
// Name-level checks only: nothing resolves and nothing is sent.

#include "policy.h"

#include <net/novaworld/gate_probe.h>

#include <cstdio>
#include <string>

using namespace opennova;
using nw_lister::DestinationPolicy;
using nw_lister::destination_permitted;

namespace {

int failures = 0;
#define CHECK(c)                                                                     \
	do {                                                                             \
		if (!(c)) {                                                                  \
			std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c);                 \
			++failures;                                                              \
		}                                                                            \
	} while (0)

} // namespace

int main() {
	// NovaLogic's domain: the name itself or one under it, any case, a trailing root dot tolerated.
	CHECK(is_novaworld_domain_host(GATE_DEFAULT_HOST));
	CHECK(is_novaworld_domain_host("gs.novaworld.net"));
	CHECK(is_novaworld_domain_host("GS.NovaWorld.NET."));
	CHECK(is_novaworld_domain_host("novaworld.net"));
	CHECK(is_novaworld_domain_host("NOVAWORLD.NET."));
	CHECK(!is_novaworld_domain_host("novaworld.net.example.com"));
	CHECK(!is_novaworld_domain_host("notnovaworld.net"));
	CHECK(!is_novaworld_domain_host("nw.opennova.example"));
	CHECK(!is_novaworld_domain_host("127.0.0.1"));
	CHECK(!is_novaworld_domain_host(""));
	CHECK(!is_novaworld_domain_host("."));

	// opennova-serve: loopback and the OpenNova service by default, NovaLogic's NovaWorld behind
	// --allow-public.
	const DestinationPolicy serve = DestinationPolicy::NovaLogicGated;
	CHECK(destination_permitted("127.0.0.1", serve, false));
	CHECK(destination_permitted("127.4.5.6", serve, false));
	CHECK(destination_permitted("localhost", serve, false));
	CHECK(destination_permitted("nw.opennova.example", serve, false)); // an arbitrary host
	CHECK(destination_permitted("203.0.113.7", serve, false));
	CHECK(destination_permitted("novaworld.net.example.com", serve, false)); // a lookalike
	CHECK(!destination_permitted("gs.novaworld.net", serve, false));
	CHECK(!destination_permitted("GS.NovaWorld.NET.", serve, false));
	CHECK(!destination_permitted("novaworld.net", serve, false));
	CHECK(destination_permitted("gs.novaworld.net", serve, true));
	CHECK(destination_permitted("GS.NovaWorld.NET.", serve, true));
	CHECK(destination_permitted("novaworld.net", serve, true));

	// opennova-nw-lister, unchanged: 127.0.0.0/8 and localhost only until --allow-public, and no
	// name at all.
	const DestinationPolicy lister = DestinationPolicy::LoopbackOnly;
	CHECK(destination_permitted("127.0.0.1", lister, false));
	CHECK(destination_permitted("localhost", lister, false));
	CHECK(!destination_permitted("nw.opennova.example", lister, false));
	CHECK(!destination_permitted("203.0.113.7", lister, false));
	CHECK(!destination_permitted("novaworld.net.example.com", lister, false));
	CHECK(!destination_permitted("gs.novaworld.net", lister, false));
	CHECK(destination_permitted("nw.opennova.example", lister, true));
	CHECK(destination_permitted("gs.novaworld.net", lister, true));

	if (failures != 0) {
		std::printf("nw_lister_destination_policy: %d failure(s)\n", failures);
		return 1;
	}
	std::printf("nw_lister_destination_policy: ok\n");
	return 0;
}
