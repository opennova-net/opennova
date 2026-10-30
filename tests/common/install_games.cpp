// The tests' registration point (games/games.h): opennova_test_games, an OBJECT library every test
// executable links (tests/CMakeLists.txt), so its object is always linked in and its one
// namespace-scope object installs every game's titles, protocols and rules before main, and the
// rules a World no session titled runs. The object runs in the library's initialization phase,
// ahead of the tests' own namespace-scope objects (a `static const Protocol &kJo =
// *title_protocol("jo")` reads the registry at its initialization).

#include <games/games.h>
#include <runtime/world/title_rules.h>

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
		// The rules a World, a Match or a GameConfig no session titled runs: Joint Operations',
		// which every fixture ran before titles had rules (world::install_unset_title_rules). The
		// programs install none, so a session that forgets its title aborts there.
		opennova::world::install_unset_title_rules(&opennova::world::title_rules("jo"));
	}
};

InstallGames install_games OPENNOVA_EARLY_INIT;

} // namespace
