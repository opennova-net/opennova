// The Joint Operations demo (jodemo.exe, the demo install's): its title and its protocol, whose
// gate tag alone is witnessed, so its session and web flow are refused by name.

#include <base/title/title.h>
#include <base/vfs/vfs_decode.h>
#include <formats/gamecfg/game_cfg.h>
#include <net/protocol/protocol.h>

#include <games/jo/jo.h>

namespace opennova::games {

namespace {

Title make_title() {
	Title t;
	t.code = "jodemo";
	t.display_name = "Joint Operations (Demo)";
	// The JO Demo stamps the SAME SCR version byte (1) as retail JO on every SCR file but keys
	// them with the DEFAULT key (0xABEEFACE), verified by decrypting demores.pff: all 214
	// version-1 payloads (.def/.adm/.aip/.ptg/.ptl/.ptu/.txt) decode cleanly under DEFAULT and to
	// garbage under JO_DFX2, and the demo ships no version-2 files. So the demo forces the
	// DEFAULT key.
	t.scr_policy = VFS_SCR_FORCE_DEFAULT;
	// The program the demo's install ships beside its archives.
	t.executable = "jodemo.exe";
	t.config_table = gamecfg::ConfigTable::Jo;
	// Unwitnessed: no binary of the title was read for its player caps (DFX2-8's sites), so it
	// keeps Joint Operations', as it runs Joint Operations' rules.
	t.max_players = jo_title().max_players;
	t.max_players_host_screen = jo_title().max_players_host_screen;
	return t;
}

Protocol make_protocol() {
	Protocol p;
	p.code = "jodemo";
	p.name = "JO demo";
	// docs/net/novaworld-net-re.md §6.7.
	p.gate_tag = "jopd:cus4";
	// Its engine build, which no protocol carries (ONE's build fills the engine-build slots):
	// "V%i.%i.%i.%i" over 1, 0, 0, 9, ebx = 1 and ebp = 0 [orig: jodemo.exe @ 0x48ad10 — ebx
	// @ 0x48ad52, ebp @ 0x48b33e, the sprintf @ 0x48b440..0x48b459, the dword 0x01000009
	// @ 0x48b44f].
	return p;
}

} // namespace

const Title &jodemo_title() {
	static const Title title = make_title();
	return title;
}

const Protocol &jodemo_protocol() {
	static const Protocol protocol = make_protocol();
	return protocol;
}

// Unwitnessed: no binary of the title was read for its in-match rules (the DFX2 catalog's sites,
// runtime/world/title_rules.h), so it runs Joint Operations' rules, the same object, as its
// session is refused by name before any rule runs.
const world::TitleRules &jodemo_rules() {
	return jo_rules();
}

} // namespace opennova::games
