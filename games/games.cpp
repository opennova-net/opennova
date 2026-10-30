#include "games.h"

#include <games/acidgambit/acidgambit.h>
#include <games/bhd/bhd.h>
#include <games/dfx/dfx.h>
#include <games/dfx2/dfx2.h>
#include <games/jo/jo.h>
#include <games/jodemo/jodemo.h>

#include <base/title/title.h>
#include <net/protocol/protocol.h>

#include <iterator>

namespace opennova::games {

void install() {
	static bool installed = false;
	if (installed)
		return;
	installed = true;
	const Title *const titles[] = {
		&jo_title(),
		&jodemo_title(),
		&dfx_title(),
		&dfx2_title(),
		&bhd_title(),
		&acidgambit_title(),
	};
	// The title a launch that names none runs as: OpenNova's own game.
	install_titles(titles, std::size(titles), &acidgambit_title());
	const TitleProtocol protocols[] = {
		{ jo_title().code, &jo_protocol() },
		{ jodemo_title().code, &jodemo_protocol() },
		{ dfx_title().code, &dfx_protocol() },
		{ dfx2_title().code, &dfx2_protocol() },
		{ bhd_title().code, &bhd_protocol() },
		{ acidgambit_title().code, &acidgambit_protocol() },
	};
	install_protocols(protocols, std::size(protocols));
}

} // namespace opennova::games
