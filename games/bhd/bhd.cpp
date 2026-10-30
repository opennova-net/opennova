// Delta Force: Black Hawk Down: its title and its protocol, neither read from a binary (no
// program, no identity), so its session and web flow are refused by name.

#include <base/title/title.h>
#include <formats/gamecfg/game_cfg.h>
#include <net/protocol/protocol.h>

#include <games/jo/jo.h>

namespace opennova::games {

namespace {

Title make_title() {
	Title t;
	t.code = "bhd";
	t.display_name = "Delta Force: Black Hawk Down";
	t.config_table = gamecfg::ConfigTable::Jo;
	// Unwitnessed: no binary of the title was read for its player caps (DFX2-8's sites), so it
	// keeps Joint Operations', as it runs Joint Operations' rules.
	t.max_players = jo_title().max_players;
	t.max_players_host_screen = jo_title().max_players_host_screen;
	return t;
}

Protocol make_protocol() {
	Protocol p;
	p.code = "bhd";
	p.name = "BHD";
	return p;
}

} // namespace

const Title &bhd_title() {
	static const Title title = make_title();
	return title;
}

const Protocol &bhd_protocol() {
	static const Protocol protocol = make_protocol();
	return protocol;
}

// Unwitnessed: no binary of the title was read for its in-match rules (the DFX2 catalog's sites,
// runtime/world/title_rules.h), so it runs Joint Operations' rules, the same object, as its
// session is refused by name before any rule runs.
const world::TitleRules &bhd_rules() {
	return jo_rules();
}

} // namespace opennova::games
