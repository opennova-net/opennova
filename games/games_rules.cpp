#include "games.h"

#include <games/acidgambit/acidgambit.h>
#include <games/bhd/bhd.h>
#include <games/dfx/dfx.h>
#include <games/dfx2/dfx2.h>
#include <games/jo/jo.h>

#include <base/title/title.h>
#include <runtime/world/title_rules.h>

#include <iterator>

namespace opennova::games {

void install_rules() {
	static bool installed = false;
	if (installed)
		return;
	installed = true;
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
