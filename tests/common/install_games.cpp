// The tests' registration point (games/games.h): opennova_test_games, an OBJECT library every test
// executable links (tests/CMakeLists.txt), so its object is always linked in and its one
// namespace-scope object installs every game's titles, protocols and rules before main (no holder
// runs a default title's: a fixture names its own, common/title_fixture.h). The object runs in
// the library's initialization phase, ahead of the tests' own namespace-scope objects (a `static
// const Protocol &kJo = *title_protocol("jo")` reads the registry at its initialization).

#include <games/games.h>

#if defined(_MSC_VER)
#pragma warning(disable : 4073) // initializers put in library initialization area
#pragma init_seg(lib)
#define OPENNOVA_EARLY_INIT
#elif defined(__GNUC__)
#define OPENNOVA_EARLY_INIT __attribute__((init_priority(101)))
#else
#define OPENNOVA_EARLY_INIT
#endif

namespace {

struct InstallGames {
	InstallGames() {
		opennova::games::install();
		opennova::games::install_rules();
	}
};

InstallGames install_games OPENNOVA_EARLY_INIT;

} // namespace
