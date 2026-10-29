// The games games::install() registers (games/): every installed title resolves and speaks a
// protocol, JO's and DFX2's complete for the session and the web flow, OP: Acid Gambit speaking
// JO's very object, the unwitnessed titles refused by name, and a PN that two titles' one
// protocol carries resolving to that one object.

#include <base/title/title.h>
#include <games/acidgambit/acidgambit.h>
#include <games/dfx2/dfx2.h>
#include <games/jo/jo.h>
#include <games/novalogic_2009/generation.h>
#include <net/protocol/protocol.h>

#include <cstdio>
#include <string>

using namespace opennova;

namespace {

int g_failures = 0;

#define CHECK(cond)                                                                                \
	do {                                                                                           \
		if (!(cond)) {                                                                             \
			std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                            \
			++g_failures;                                                                          \
		}                                                                                          \
	} while (0)

// Every installed title resolves by its code and speaks an installed protocol.
void every_title_resolves() {
	CHECK(title_count() == 6);
	for (size_t i = 0; i < title_count(); ++i) {
		const Title *title = title_at(i);
		CHECK(title != nullptr && title_by_code(title->code) == title);
		const Protocol *protocol = title != nullptr ? title_protocol(title->code) : nullptr;
		CHECK(protocol != nullptr && protocol->code != nullptr && protocol->name != nullptr);
		if (protocol != nullptr)
			CHECK(protocol_by_code(protocol->code) == protocol);
	}
	CHECK(title_protocol("nosuchgame") == nullptr && protocol_by_code("nosuchgame") == nullptr);
	CHECK(&default_title() == title_by_code("jo"));
}

// JO's and DFX2's protocols are complete for the session and the web flow, each the games'
// accessor's object over the 2009 generation, each named for its protocol line.
void retail_protocols_complete() {
	const Protocol *jo = title_protocol("jo");
	const Protocol *dfx2 = title_protocol("dfx2");
	CHECK(jo == &games::jo_protocol() && dfx2 == &games::dfx2_protocol());
	CHECK(&games::jo_title() == title_by_code("jo") &&
			&games::dfx2_title() == title_by_code("dfx2"));
	for (const Protocol *protocol : { jo, dfx2 }) {
		CHECK(protocol != nullptr);
		if (protocol == nullptr)
			continue;
		CHECK(protocol_complete(*protocol, ProtocolPart::Session));
		CHECK(protocol_complete(*protocol, ProtocolPart::WebFlow));
		CHECK(protocol->generation == &games::novalogic_2009());
		CHECK(std::string(protocol_version(*protocol)) == "16");
		std::string error;
		CHECK(resolve_title_protocol(protocol->code, error) == protocol && error.empty());
	}
	CHECK(std::string(jo->name) == "JO" && std::string(dfx2->name) == "DFX2");
	CHECK(jo != dfx2);
}

// OP: Acid Gambit speaks JO's protocol: the same object, no copied literals, so a
// JOINTOPERATIONS peer resolves to the one protocol whichever title registered it.
void acidgambit_speaks_jo() {
	const Protocol *jo = title_protocol("jo");
	CHECK(title_protocol("acidgambit") == jo);
	CHECK(&games::acidgambit_protocol() == &games::jo_protocol());
	CHECK(protocol_by_pn("JOINTOPERATIONS") == jo && protocol_by_pn("jointoperations") == jo);
	std::string error;
	CHECK(resolve_title_protocol("acidgambit", error) == jo);
	const Title &ag = games::acidgambit_title();
	CHECK(title_by_code("ACIDGAMBIT") == &ag);
	CHECK(std::string(ag.display_name) == "OP: Acid Gambit" &&
			std::string(ag.title_text) == "OP: Acid Gambit");
	CHECK(std::string(ag.command_file) == "ACIDGAMBIT.CMD" && ag.executable == nullptr);
	CHECK(ag.scr_policy == games::jo_title().scr_policy);
	CHECK(protocol_by_pn("DELTAFORCEXTREME2") == title_protocol("dfx2"));
}

// The titles no binary has been read for: refused by name, as before the registry.
void unwitnessed_titles_refused() {
	std::string error;
	CHECK(resolve_title_protocol("bhd", error) == nullptr);
	CHECK(error.find("the bhd title's session identity is not witnessed yet: version text, NVS") ==
			0);
	CHECK(resolve_title_protocol("dfx", error) == nullptr);
	CHECK(error.find("the dfx title's session identity is not witnessed yet: NVS") == 0);
	CHECK(resolve_title_protocol("jodemo", error) == nullptr);
	CHECK(error.find("PG") != std::string::npos && error.find("gate tag") == std::string::npos);
	const Protocol *demo = title_protocol("jodemo");
	CHECK(demo != nullptr && std::string(demo->gate_tag) == "jopd:cus4");
	CHECK(protocol_refusal(*demo, ProtocolPart::WebFlow) ==
			"the jodemo title's web flow identity is not witnessed yet: sign-in submit");
	CHECK(resolve_title_protocol("nosuchgame", error) == nullptr);
	CHECK(error == "unknown game 'nosuchgame'");
	// The version texts the menus show: each title's binary's own, the default title's for an
	// empty or unknown code, none for one whose binary is unread.
	CHECK(std::string(title_version_text("jodemo")) == "V1.0.0.9");
	CHECK(std::string(title_version_text("dfx")) == "V1.7.1.9");
	CHECK(std::string(title_version_text("")) == "V1.7.5.7");
	CHECK(title_version_text("bhd") == nullptr);
}

// A version line's title parts (title_versions): the title's name where the line names no game,
// the protocol it speaks with its PV2 where its session part is witnessed; OP: Acid Gambit's is
// JO's; an unwitnessed protocol and an unknown title add no protocol.
void title_parts_of_the_version_line() {
	Versions jo;
	title_versions("jo", jo);
	CHECK(jo.game == "Joint Operations" && jo.protocol == "JO" && jo.protocol_version == "16");
	Versions dfx2;
	title_versions("DFX2", dfx2);
	CHECK(dfx2.game == "Delta Force Xtreme 2" && dfx2.protocol == "DFX2" &&
			dfx2.protocol_version == "16");
	Versions named;
	named.game = "My Game";
	title_versions("acidgambit", named);
	CHECK(named.game == "My Game" && named.protocol == "JO" && named.protocol_version == "16");
	Versions demo;
	title_versions("jodemo", demo);
	CHECK(demo.game == "Joint Operations (Demo)" && demo.protocol.empty() &&
			demo.protocol_version.empty());
	Versions unknown;
	title_versions("nosuchgame", unknown);
	title_versions("", unknown);
	CHECK(unknown.game.empty() && unknown.protocol.empty());
}

} // namespace

int main() {
	every_title_resolves();
	retail_protocols_complete();
	acidgambit_speaks_jo();
	unwitnessed_titles_refused();
	title_parts_of_the_version_line();
	if (g_failures == 0) {
		std::printf("games: all checks passed\n");
		return 0;
	}
	std::printf("games: %d check(s) failed\n", g_failures);
	return 1;
}
