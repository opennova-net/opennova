#include "acidgambit.h"

#include <games/jo/jo.h>

#include <formats/gamecfg/game_cfg.h>

namespace opennova::games {

namespace {

Title make() {
	Title t;
	t.code = "acidgambit";
	t.display_name = "OP: Acid Gambit";
	t.title_text = "OP: Acid Gambit";
	// No program of its own: the runtime runs it.
	t.executable = nullptr;
	// The command file its own wizard names, the one the release build writes into its package
	// (products/release_build.h).
	t.command_file = "ACIDGAMBIT.CMD";
	// The login its wizard names, the service's product for it (products.updater_login).
	t.updater_login = "ACIDGAMBIT";
	t.config_table = gamecfg::ConfigTable::Jo;
	// Its player caps are Joint Operations', as its rules are (acidgambit_rules.cpp).
	t.max_players = jo_title().max_players;
	t.max_players_host_screen = jo_title().max_players_host_screen;
	t.host_screen = jo_title().host_screen;
	// Its expansions and the .def names it opens are Joint Operations', as its code is.
	t.expansions = jo_title().expansions;
	t.read_defs = jo_title().read_defs;
	// Its HUD's elements and fonts are Joint Operations', as its HUD is.
	t.hud_exp_points = jo_title().hud_exp_points;
	t.hud_lone_gunner_panel = jo_title().hud_lone_gunner_panel;
	t.hud_waypoint_active_tint = jo_title().hud_waypoint_active_tint;
	t.hud_fonts = jo_title().hud_fonts;
	// Its mission catalog's order is Joint Operations', as its code is.
	t.mission_list_order = jo_title().mission_list_order;
	// Its profile files are Joint Operations', as its code is.
	t.profile_magic = jo_title().profile_magic;
	// Its command line is Joint Operations', /maxplayers with it.
	t.max_players_switch = jo_title().max_players_switch;
	// Its fresh profile's kit pages are Joint Operations', as its code is.
	t.profile_single_player_kit = jo_title().profile_single_player_kit;
	t.profile_class_kits = jo_title().profile_class_kits;
	t.mod_list_base = jo_title().mod_list_base;
	return t;
}

} // namespace

const Title &acidgambit_title() {
	static const Title title = make();
	return title;
}

} // namespace opennova::games
