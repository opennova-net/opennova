#include "games.h"

#include <games/acidgambit/acidgambit.h>
#include <games/dfx2/dfx2.h>
#include <games/jo/jo.h>

#include <base/title/title.h>
#include <net/protocol/protocol.h>
#include <runtime/world/title_rules.h>

#include <iterator>

namespace opennova::games {

// The unwitnessed titles, one TU each (games/dfx, games/bhd).
const Title &dfx_title();
const Protocol &dfx_protocol();
const world::TitleRules &dfx_rules();
const Title &bhd_title();
const Protocol &bhd_protocol();
const world::TitleRules &bhd_rules();

void install() {
	static bool installed = false;
	if (installed)
		return;
	installed = true;
	const Title *const titles[] = {
		&jo_title(),
		&dfx_title(),
		&dfx2_title(),
		&bhd_title(),
		&acidgambit_title(),
	};
	// The title a launch that names none runs as: OpenNova's own game.
	install_titles(titles, std::size(titles), &acidgambit_title());
	const TitleProtocol protocols[] = {
		{ jo_title().code, &jo_protocol() },
		{ dfx_title().code, &dfx_protocol() },
		{ dfx2_title().code, &dfx2_protocol() },
		{ bhd_title().code, &bhd_protocol() },
		{ acidgambit_title().code, &acidgambit_protocol() },
	};
	install_protocols(protocols, std::size(protocols));
	const world::TitleRulesRow rules[] = {
		{ jo_title().code, &jo_rules() },
		{ dfx_title().code, &dfx_rules() },
		{ dfx2_title().code, &dfx2_rules() },
		{ bhd_title().code, &bhd_rules() },
		{ acidgambit_title().code, &acidgambit_rules() },
	};
	world::install_title_rules(rules, std::size(rules));
}

} // namespace opennova::games
