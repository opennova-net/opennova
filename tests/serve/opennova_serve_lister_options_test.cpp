// opennova-serve's NovaWorld listing inputs. The destination policy
// (apps/serve/destination_policy.h, ADR 0051 d6): loopback and any host off NovaLogic's domain by
// default, a name under novaworld.net only with --allow-public [orig: CNapiGateManager_Init
// @0x633f90 (host "novaworld.net"); "gs.novaworld.net" @0x7cc368]; name-level checks only, nothing
// resolves and nothing is sent. Then the --credentials reader (apps/serve/credentials.h): KEY=VALUE
// lines, comments, quoted and bare values.

#include "credentials.h"
#include "destination_policy.h"

#include <net/novaworld/gate_probe.h>

#include "common/temp_dir.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

using namespace opennova;
using serve::destination_permitted;

namespace {

int failures = 0;
#define CHECK(c)                                                                                   \
	do {                                                                                           \
		if (!(c)) {                                                                                \
			std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c);                               \
			++failures;                                                                            \
		}                                                                                          \
	} while (0)

std::string write_temp(const test_temp::TempDir &dir, const char *name, const std::string &text) {
	const std::string path = dir.file(name);
	std::ofstream(opennova::io::os_path(path), std::ios::binary) << text;
	return path;
}

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

	// Loopback and the OpenNova service by default, NovaLogic's NovaWorld behind --allow-public.
	CHECK(destination_permitted("127.0.0.1", false));
	CHECK(destination_permitted("127.4.5.6", false));
	CHECK(destination_permitted("localhost", false));
	CHECK(destination_permitted("nw.opennova.example", false)); // an arbitrary host
	CHECK(destination_permitted("203.0.113.7", false));
	CHECK(destination_permitted("novaworld.net.example.com", false)); // a lookalike
	CHECK(!destination_permitted("gs.novaworld.net", false));
	CHECK(!destination_permitted("GS.NovaWorld.NET.", false));
	CHECK(!destination_permitted("novaworld.net", false));
	CHECK(destination_permitted("gs.novaworld.net", true));
	CHECK(destination_permitted("GS.NovaWorld.NET.", true));
	CHECK(destination_permitted("novaworld.net", true));

	// The credentials file: a comment, a quoted value and a bare one; a key it does not know is
	// ignored.
	const test_temp::TempDir dir("opennova_serve_lister_options");
	{
		const std::string path = write_temp(dir, "creds_test.txt",
				"# comment\nNOVAWORLD_USER = \"someone\"\nNOVAWORLD_PASS=secret\nOTHER_KEY=x\n");
		serve::Credentials c;
		std::string err;
		CHECK(serve::load_credentials(path, c, err));
		CHECK(c.user == "someone" && c.pass == "secret" && c.present());
	}
	{
		const std::string path = write_temp(
				dir, "creds_half.txt", "; no password\nNOVAWORLD_USER='solo'\n");
		serve::Credentials c;
		std::string err;
		CHECK(serve::load_credentials(path, c, err));
		CHECK(c.user == "solo" && c.pass.empty() && !c.present());
	}
	{
		serve::Credentials c;
		std::string err;
		CHECK(!serve::load_credentials(dir.file("creds_missing.txt"), c, err) && !err.empty());
	}

	if (failures != 0) {
		std::printf("opennova_serve_lister_options: %d failure(s)\n", failures);
		return 1;
	}
	std::printf("opennova_serve_lister_options: ok\n");
	return 0;
}
