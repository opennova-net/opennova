#include "acidgambit.h"

#include <games/jo/jo.h>

namespace opennova::games {

namespace {

Title make() {
	// Joint Operations' row, as its code is Joint Operations' (its rules and its protocol are JO's
	// very objects: acidgambit_rules.cpp, acidgambit_protocol.cpp): every field past its identity
	// is jo_title()'s, as dfx2_protocol.cpp's sheet starts from jo_protocol(), so a value JO's row
	// sets is its value too (games_test compares every field past the identity).
	Title t = jo_title();
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
	return t;
}

} // namespace

const Title &acidgambit_title() {
	static const Title title = make();
	return title;
}

} // namespace opennova::games
