#include "games.h"

#include <games/acidgambit/acidgambit.h>
#include <games/dfx2/dfx2.h>
#include <games/jo/jo.h>

#include <base/title/title.h>
#include <net/protocol/protocol.h>

#include <iterator>

namespace opennova::games {

// The unwitnessed titles, one TU each (games/jodemo, games/dfx, games/bhd).
const Title &jodemo_title();
const Protocol &jodemo_protocol();
const Title &dfx_title();
const Protocol &dfx_protocol();
const Title &bhd_title();
const Protocol &bhd_protocol();

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
	// The title a launch that names none runs as: Joint Operations'.
	install_titles(titles, std::size(titles), &jo_title());
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
