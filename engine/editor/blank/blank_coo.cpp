#include "blank_makers.h"

#include <net/novaworld/cookie_jar_file.h>

namespace opennova::editor {

// nw_cdata.coo is the menu browser's cookie jar file: the word the loader skips, the magic, then
// one record per saved cookie, every string encrypted under a key made from this computer's volume
// (write_cookie_jar, engine/net/novaworld/cookie_jar_file.h). A record written on one machine
// does not decrypt on another, so the only portable file is the empty jar, written as the game
// writes one: the leading word 1 (no REMEMBER_LOGIN checkbox set yet) and the magic, no records.
// [orig: CUIScene_SaveCookieJarFile @0x63BA60; CookieJar_SaveToFile @0x64EE60]
bool make_blank_coo(const BlankRequest &, std::vector<uint8_t> &out, Diagnostic &) {
	out = write_cookie_jar(CookieJar{}, /*remember_login_unchecked=*/false, std::string_view());
	return true;
}

} // namespace opennova::editor
