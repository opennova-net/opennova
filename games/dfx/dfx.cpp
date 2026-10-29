// Delta Force: Xtreme (DFX.EXE, the project's IDB's): its title and its protocol, whose version
// text alone is witnessed (no binary of it was read for its identity), so its session and web
// flow are refused by name.

#include <base/title/title.h>
#include <base/vfs/vfs_decode.h>
#include <formats/gamecfg/game_cfg.h>
#include <net/protocol/protocol.h>

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
	return t;
}

Protocol make_protocol() {
	Protocol p;
	p.code = "dfx";
	p.name = "DFX";
	// The version text: 1, 7, 1, 9, ebx = 1 [orig: DFX.EXE @ 0x4a6180 — ebx @ 0x4a61eb, the
	// sprintf @ 0x4a6b93..0x4a6bad, the dword 0x01070109 @ 0x4a6ba3].
	p.version_text = "V1.7.1.9";
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

} // namespace opennova::games
