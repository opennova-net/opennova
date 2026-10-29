// Delta Force: Black Hawk Down: its title and its protocol, neither read from a binary (no
// version text, no program, no identity), so its session and web flow are refused by name.

#include <base/title/title.h>
#include <base/vfs/vfs_decode.h>
#include <formats/gamecfg/game_cfg.h>
#include <net/protocol/protocol.h>

namespace opennova::games {

namespace {

Title make_title() {
	Title t;
	t.code = "bhd";
	t.display_name = "Delta Force: Black Hawk Down";
	t.scr_policy = VFS_SCR_VERSION_DETECT;
	t.config_table = gamecfg::ConfigTable::Jo;
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

} // namespace opennova::games
