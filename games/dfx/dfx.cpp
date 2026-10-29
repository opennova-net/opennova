// Delta Force: Xtreme (DFX.EXE, the project's IDB's): its title and its protocol, of which no
// identity is witnessed (only its engine build was read), so its session and web flow are refused
// by name.

#include <base/title/title.h>
#include <base/vfs/vfs_decode.h>
#include <formats/gamecfg/game_cfg.h>
#include <net/protocol/protocol.h>

#include <games/jo/jo.h>

namespace opennova::games {

namespace {

Title make_title() {
	Title t;
	t.code = "dfx";
	t.display_name = "Delta Force: Xtreme";
	t.scr_policy = VFS_SCR_VERSION_DETECT;
	// The program the DFX install ships beside its archives.
	t.executable = "DFX.EXE";
	t.config_table = gamecfg::ConfigTable::Jo;
	// Unwitnessed: no binary of the title was read for its player caps (DFX2-8's sites), so it
	// keeps Joint Operations', as it runs Joint Operations' rules.
	t.max_players = jo_title().max_players;
	t.max_players_host_screen = jo_title().max_players_host_screen;
	return t;
}

Protocol make_protocol() {
	Protocol p;
	p.code = "dfx";
	p.name = "DFX";
	// Its engine build, which no protocol carries (ONE's build fills the engine-build slots):
	// "V%i.%i.%i.%i" over 1, 7, 1, 9, ebx = 1 [orig: DFX.EXE @ 0x4a6180 — ebx @ 0x4a61eb, the
	// sprintf @ 0x4a6b93..0x4a6bad, the dword 0x01070109 @ 0x4a6ba3].
	return p;
}

} // namespace

const Title &dfx_title() {
	static const Title title = make_title();
	return title;
}

const Protocol &dfx_protocol() {
	static const Protocol protocol = make_protocol();
	return protocol;
}

// Unwitnessed: no binary of the title was read for its in-match rules (the DFX2 catalog's sites,
// runtime/world/title_rules.h), so it runs Joint Operations' rules, the same object, as its
// session is refused by name before any rule runs.
const world::TitleRules &dfx_rules() {
	return jo_rules();
}

} // namespace opennova::games
