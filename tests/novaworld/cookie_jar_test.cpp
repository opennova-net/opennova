// The menu browser's cookie jar as retail keeps it (D-NET-NEW-L9-58): the Set-Cookie parse off the
// answer's header text, the store's match (the name case-insensitively, then the path rule) and the
// by-name read. And its file across processes, nw_cdata.coo (D-NET-NEW-L9-59): the volume key, the
// writer's bytes, the PERSISTENT* filter, the REMEMBER_LOGIN rule, the reader, the menu browser
// state's create and save.
// [orig: HTTP_ResponseHandler @0x659480; HttpCookie_Parse @0x658F60; CookieJar_UpdateFromURL
//  @0x64E630; CookieJar_GetValueByName @0x64EA90; String_CompareNoCase @0x650000;
//  CDKey_GenerateVolumeFingerprint @0x651ED0; Crypto_EncryptBuffer @0x437510;
//  CUIScene_SaveCookieJarFile @0x63BA60; CookieJar_SaveToFile @0x64EE60;
//  CUIStringTable_OpenAndLoad @0x63A500; CUIStringTable_LoadFromFile @0x64F290]

#include <net/novaworld/cookie_jar_file.h>
#include <net/novaworld/http_flow.h>
#include <net/novaworld/http_login.h>
#include <net/novacrypto/epask.h>
#include <net/novacrypto/pubcrypto.h>
#include <net/novacrypto/url_cipher.h>
#include <net/title/title_identity.h>

#include <base/gameprofile/gameprofile.h>

#include <base/io/file_io.h>

#include "common/temp_dir.h"
#include "common/test_expect.h"

#include <array>
#include <optional>
#include <string>
#include <vector>

using opennova::CookieEntry;
using opennova::CookieJar;
using opennova::parse_set_cookie_headers;

namespace {

CookieEntry entry(const std::string &name, const std::string &value,
		std::optional<std::string> path = std::nullopt) {
	CookieEntry e;
	e.name = name;
	e.value = value;
	e.path = std::move(path);
	return e;
}

std::vector<std::string> header_names(const CookieJar &jar) {
	std::vector<std::string> names;
	for (const CookieEntry &e : jar.entries()) names.push_back(e.name);
	return names;
}

// Our service's shape (every cookie "; Path=/"), and a value that stops at its ';'.
int test_parse_path() {
	const std::vector<CookieEntry> parsed = parse_set_cookie_headers(
			{"Content-Type: text/html", "Set-Cookie: NWHANDLE=Walker; Path=/", "Connection: close"});
	TEST_EXPECT(parsed.size() == 1);
	TEST_EXPECT(parsed[0].name == "NWHANDLE" && parsed[0].value == "Walker");
	TEST_EXPECT(parsed[0].path && *parsed[0].path == "/");
	TEST_EXPECT(!parsed[0].expires && !parsed[0].domain && parsed[0].secure == 0);
	// The header's name is matched anywhere and in any case; "SET-COOKIE" plus two bytes is skipped,
	// then spaces and colons.
	const std::vector<CookieEntry> lower = parse_set_cookie_headers({"set-cookie:  A=1; domain=x.test"});
	TEST_EXPECT(lower.size() == 1 && lower[0].name == "A" && lower[0].value == "1");
	TEST_EXPECT(lower[0].domain && *lower[0].domain == "x.test" && !lower[0].path);
	// A name with no '=' before the line's CR refuses the cookie.
	TEST_EXPECT(parse_set_cookie_headers({"Set-Cookie: NOVALUE", "X: y"}).empty());
	return 0;
}

// A cookie with no attribute walks on into the next header line as its attribute text: a following
// "Expires:" header names EXPIRES with no '=' and refuses the cookie; any other line passes.
int test_parse_walks_into_the_next_line() {
	TEST_EXPECT(parse_set_cookie_headers(
			{"Set-Cookie: A=1", "Expires: Thu, 01 Jan 1970 00:00:00 GMT"}).empty());
	const std::vector<CookieEntry> kept =
			parse_set_cookie_headers({"Set-Cookie: A=1", "Content-Type: text/html"});
	TEST_EXPECT(kept.size() == 1 && kept[0].name == "A" && kept[0].value == "1" && !kept[0].path);
	// The last line's cookie with no attribute keeps its entry.
	const std::vector<CookieEntry> last = parse_set_cookie_headers({"X: y", "Set-Cookie: B=2"});
	TEST_EXPECT(last.size() == 1 && last[0].name == "B" && last[0].value == "2");
	return 0;
}

// EXPIRES keeps the 29 bytes from its own name and moves the walk 29 bytes past its value: a path
// after it is jumped over. SECURE sets the word and moves the walk 6 bytes past its name's end, into
// the next line here, whose own cookie still parses whole.
int test_parse_expires_and_secure() {
	const std::vector<CookieEntry> parsed = parse_set_cookie_headers(
			{"Set-Cookie: P=1; expires=Thu, 01-Jan-2037 00:00:00 GMT; path=/x",
			 "Content-Type: text/html"});
	TEST_EXPECT(parsed.size() == 1 && parsed[0].name == "P" && parsed[0].value == "1");
	TEST_EXPECT(parsed[0].expires && *parsed[0].expires == "expires=Thu, 01-Jan-2037 00:0");
	TEST_EXPECT(!parsed[0].path);
	const std::vector<CookieEntry> secure =
			parse_set_cookie_headers({"Set-Cookie: S=1; secure", "Set-Cookie: T=2; path=/"});
	TEST_EXPECT(secure.size() == 2);
	TEST_EXPECT(secure[0].name == "S" && secure[0].secure == 1 && !secure[0].path);
	TEST_EXPECT(secure[1].name == "T" && secure[1].path && *secure[1].path == "/" && secure[1].secure == 0);
	return 0;
}

// The store: an empty jar takes the batch whole; after that the first entry whose name matches
// case-insensitively and whose path matches takes the value, keeping its own name and path; an
// entry that matches none is appended, and a later entry of the batch can update it.
int test_store() {
	CookieJar jar;
	jar.update({entry("A", "1", std::string("/")), entry("A", "2", std::string("/"))});
	TEST_EXPECT(jar.entries().size() == 2 && *jar.find("a") == "1");

	jar.update({entry("a", "3", std::string("/")), entry("B", "4"), entry("b", "5")});
	TEST_EXPECT(header_names(jar) == (std::vector<std::string>{"A", "A", "B"}));
	TEST_EXPECT(jar.entries()[0].value == "3" && jar.entries()[1].value == "2");
	TEST_EXPECT(jar.entries()[2].value == "5" && !jar.entries()[2].path);

	// The path rule: the same path (case-insensitively); "/" against a null or empty path either
	// way; a null path against "" does not match, nor does another path.
	CookieJar paths;
	paths.update({entry("R", "root", std::string("/")), entry("E", "empty", std::string("")),
			entry("X", "other", std::string("/x"))});
	paths.set("R", "set");    // null against "/": updated
	paths.set("E", "set");    // null against "": appended
	paths.set("X", "set");    // null against "/x": appended
	paths.update({entry("x", "upper", std::string("/X"))}); // "/X" against "/x": updated
	TEST_EXPECT(header_names(paths) == (std::vector<std::string>{"R", "E", "X", "E", "X"}));
	TEST_EXPECT(paths.entries()[0].value == "set" && paths.entries()[1].value == "empty");
	TEST_EXPECT(paths.entries()[2].value == "upper" && paths.entries()[3].value == "set");
	TEST_EXPECT(paths.entries()[4].value == "set" && *paths.find("e") == "empty");
	CookieJar null_first;
	null_first.set("N", "a");
	null_first.update({entry("N", "b", std::string("/"))}); // "/" against null: updated
	TEST_EXPECT(null_first.entries().size() == 1 && null_first.entries()[0].value == "b" &&
	            !null_first.entries()[0].path);

	// Every entry rides the request, duplicates included, in the jar's order.
	TEST_EXPECT(paths.cookie_header_lines() ==
	            (std::vector<std::string>{"R=set;", "E=empty;", "X=upper;", "E=set;", "X=set;"}));
	return 0;
}


// The volume key: the arithmetic of CDKey_GenerateVolumeFingerprint over a chosen volume (serial
// 0x1234ABCD, maximum component length 255, flags 0x03E700FF, "GAMES" on NTFS), restated from the
// disassembly into these bytes; the key is the bytes to the first zero.
opennova::RetailMachineInputs chosen_volume() {
	opennova::RetailMachineInputs volume;
	volume.volume_serial = 0x1234ABCDu;
	volume.maximum_component_length = 255;
	volume.filesystem_flags = 0x03E700FFu;
	volume.volume_name = "GAMES";
	volume.filesystem_name = "NTFS";
	return volume;
}

int test_volume_key() {
	opennova::RetailMachineInputs volume = chosen_volume();
	const std::array<uint8_t, 16> expected = {0xF5, 0x8C, 0x0C, 0x30, 0x20, 0xE9, 0x77, 0x56,
			0x76, 0xE8, 0x91, 0x50, 0xF9, 0x98, 0x06, 0x23};
	TEST_EXPECT(opennova::volume_fingerprint(volume) == expected);
	TEST_EXPECT(opennova::cookie_jar_key(volume) == std::string(expected.begin(), expected.end()));
	// A name byte that zeroes byte 5 cuts the key there (the cipher reads the key's strlen).
	volume.volume_name = std::string("GAMES") + static_cast<char>(0xE9);
	TEST_EXPECT(opennova::volume_fingerprint(volume)[5] == 0);
	const std::array<uint8_t, 5> cut = {0xF5, 0x8C, 0x0C, 0x30, 0x20};
	TEST_EXPECT(opennova::cookie_jar_key(volume) == std::string(cut.begin(), cut.end()));
	// A volume that reads nothing (off Windows): "00000000" twice over, nothing else.
	TEST_EXPECT(opennova::cookie_jar_key(opennova::RetailMachineInputs{}) == std::string(16, '0'));
	return 0;
}

// The file the writer makes for saved_jar(), restated byte by byte from the cited writer and cipher
// (the leading word 1, the magic "2OOC", one record per PERSISTENT* entry: the node word 0, five
// length-prefixed strings under the chosen volume's key, a null one a length of 0, then the
// entry's word).
const std::vector<uint8_t> kTwoRecordFile = {
		0x01, 0x00, 0x00, 0x00, 0x32, 0x4F, 0x4F, 0x43, 0x00, 0x00, 0x00, 0x00,
		0x1B, 0x00, 0x00, 0x00, 0xAF, 0x89, 0xA4, 0x57, 0x82, 0x1F, 0xB4, 0x5C,
		0xF5, 0x6B, 0xE7, 0x62, 0xD0, 0x0A, 0x53, 0xB2, 0x40, 0x09, 0x31, 0xD8,
		0x08, 0xA9, 0x36, 0xE0, 0x7B, 0xDE, 0x72, 0x10, 0x00, 0x00, 0x00, 0x50,
		0xA2, 0xD3, 0x03, 0x59, 0xC9, 0x7F, 0x9C, 0x3B, 0x4B, 0xDC, 0x62, 0xF1,
		0x7C, 0xDB, 0x53, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x1A, 0x00, 0x00, 0x00, 0x06, 0x17, 0x84, 0x78, 0xF4, 0x56, 0xC4, 0x38,
		0x75, 0xD5, 0x23, 0x48, 0x67, 0x76, 0xB5, 0xF3, 0x99, 0x91, 0x08, 0x08,
		0x79, 0xD6, 0x50, 0xBB, 0xEE, 0x52, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00,
};

CookieJar saved_jar() {
	CookieJar jar;
	jar.update({entry("NWHANDLE", "Walker", std::string("/")),
			entry("PERSISTENTREMEMBERLOGINDATA", "ABCDEFGHIJKLMNOP", std::string("/")),
			entry("LOGINSESSIONTAG", "t", std::string("/")),
			entry("PERSISTENTEXPRESSLOGINDATA", "", std::string("/"))});
	return jar;
}

// The PERSISTENT* filter and the REMEMBER_LOGIN rule: only names beginning PERSISTENT
// (case-insensitively) are saved; PERSISTENTEXPRESSLOGINDATA only while REMEMBER_LOGIN is not
// "UNCHECKED", the leading word saying which.
int test_writer() {
	const std::string key = opennova::cookie_jar_key(chosen_volume());
	const CookieJar jar = saved_jar();
	TEST_EXPECT(opennova::write_cookie_jar(jar, false, key) == kTwoRecordFile);
	const std::vector<uint8_t> unchecked = opennova::write_cookie_jar(jar, true, key);
	TEST_EXPECT(unchecked.size() < kTwoRecordFile.size() && unchecked[0] == 0);
	CookieJar read_back;
	opennova::read_cookie_jar(unchecked.data(), unchecked.size(), key, read_back);
	TEST_EXPECT(read_back.entries().size() == 1 &&
	            read_back.entries()[0].name == "PERSISTENTREMEMBERLOGINDATA");
	CookieJar lower;
	lower.update({entry("persistentNote", "x")});
	CookieJar lower_back;
	const std::vector<uint8_t> lower_file = opennova::write_cookie_jar(lower, false, key);
	opennova::read_cookie_jar(lower_file.data(), lower_file.size(), key, lower_back);
	TEST_EXPECT(lower_back.entries().size() == 1 && lower_back.entries()[0].value == "x");
	// The empty jar: the word and the magic alone (the editor's blank).
	TEST_EXPECT(opennova::write_cookie_jar(CookieJar{}, false, key) ==
	            (std::vector<uint8_t>{1, 0, 0, 0, '2', 'O', 'O', 'C'}));
	return 0;
}

// The reader appends the records as they are, every string text again (a null one ""); another
// volume's key reads other bytes; a short, foreign or truncated file adds nothing past its whole
// records.
int test_reader() {
	const std::string key = opennova::cookie_jar_key(chosen_volume());
	CookieJar jar;
	jar.set("YOURIP", "203.0.113.7");
	opennova::read_cookie_jar(kTwoRecordFile.data(), kTwoRecordFile.size(), key, jar);
	TEST_EXPECT(jar.entries().size() == 3 && jar.entries()[0].name == "YOURIP");
	const CookieEntry &remembered = jar.entries()[1];
	TEST_EXPECT(remembered.name == "PERSISTENTREMEMBERLOGINDATA" && remembered.value == "ABCDEFGHIJKLMNOP");
	TEST_EXPECT(remembered.path && *remembered.path == "/" && remembered.expires &&
	            remembered.expires->empty() && remembered.domain && remembered.domain->empty());
	TEST_EXPECT(jar.entries()[2].name == "PERSISTENTEXPRESSLOGINDATA" && jar.entries()[2].value.empty());

	CookieJar other;
	opennova::read_cookie_jar(kTwoRecordFile.data(), kTwoRecordFile.size(), std::string(16, '0'), other);
	TEST_EXPECT(other.entries().size() == 2 && other.entries()[0].name != "PERSISTENTREMEMBERLOGINDATA");

	CookieJar none;
	std::vector<uint8_t> foreign = kTwoRecordFile;
	foreign[4] = 'R';
	opennova::read_cookie_jar(foreign.data(), foreign.size(), key, none);
	const std::vector<uint8_t> blank = {0, 0, 0, 0, 'R', 'S', 'T', 'R'};
	opennova::read_cookie_jar(blank.data(), blank.size(), key, none);
	opennova::read_cookie_jar(kTwoRecordFile.data(), 7, key, none);
	TEST_EXPECT(none.empty());
	CookieJar truncated;
	opennova::read_cookie_jar(kTwoRecordFile.data(), kTwoRecordFile.size() - 1, key, truncated);
	TEST_EXPECT(truncated.entries().size() == 1);
	return 0;
}

// The menu browser state: its save writes the file under the REMEMBER_LOGIN property, its creation
// drops the property, empties the jar and loads the file; the LOGIN mark is not the scene's.
int test_menu_browser_state() {
	test_temp::TempDir dir("cookie_jar_state");
	const std::string path = dir.file("nw_cdata.coo");
	const std::string key = opennova::cookie_jar_key(chosen_volume());
	opennova::MenuBrowserState state;
	std::string error;
	TEST_EXPECT(state.save(error)); // no file: nothing written
	state.create(path, key);
	TEST_EXPECT(state.jar.empty() && !state.remember_login_checked);
	state.jar = saved_jar();
	state.remember_login_checked = false;
	state.login_button_pressed = true;
	TEST_EXPECT(state.save(error));
	std::vector<uint8_t> bytes;
	TEST_EXPECT(opennova::io::read_file_bytes(path, bytes, error));
	TEST_EXPECT(bytes == opennova::write_cookie_jar(saved_jar(), true, key));
	state.remember_login_checked = true;
	TEST_EXPECT(state.save(error));
	state.create(path, key);
	TEST_EXPECT(!state.remember_login_checked && state.login_button_pressed);
	TEST_EXPECT(state.jar.entries().size() == 2 &&
	            *state.jar.find("PERSISTENTREMEMBERLOGINDATA") == "ABCDEFGHIJKLMNOP" &&
	            state.jar.find("NWHANDLE") == nullptr);
	// The reloaded cookie's path is "/" as written, so the browser's write of the name (a null
	// path) still updates it in place.
	state.jar.set("PERSISTENTREMEMBERLOGINDATA", "");
	TEST_EXPECT(state.jar.entries().size() == 2 && state.jar.find("PERSISTENTREMEMBERLOGINDATA")->empty());
	return 0;
}


std::vector<uint8_t> page(const std::string &text) {
	return std::vector<uint8_t>(text.begin(), text.end());
}

// The jar the file at `path` holds under `key` (empty when there is no file).
CookieJar saved_at(const std::string &path, const std::string &key) {
	CookieJar jar;
	std::vector<uint8_t> file;
	std::string error;
	if (opennova::io::read_file_bytes(path, file, error)) opennova::read_cookie_jar(file.data(), file.size(), key, jar);
	return jar;
}

// The flow's saves on the browser state it shares: the home page's showing (a login's success), a
// connect page ahead of its parse (the .joi here; a relay page is none); the REMEMBER_LOGIN property
// the sign-in page's activation sets from game.cfg's rememberlogin, which the next save reads; a
// session's reset keeping the jar.
// [orig: UI_SaveCookieJarOnHomeTabs @0x557530 - the save called @0x55753B; UI_ProcessWebResponseContent
//  @0x63D450 - event 7 raised @0x63D634; UI_DispatchScreenEvent @0x54E6A0 - case 7's save called
//  @0x54F2C5; UI_LoadPersistentLoginCredentials @0x557330 - CCheckWnd_SetCheckedState called
//  @0x55744A; Menu_InitShellResources @0x552500 - the jar made once (@0x5525A6)]
int test_flow_save_sites() {
	test_temp::TempDir dir("cookie_jar_flow");
	const std::string path = dir.file("nw_cdata.coo");
	const std::string key = opennova::cookie_jar_key(chosen_volume());
	opennova::MenuBrowserState state;
	state.create(path, key);
	opennova::LobbyHttpContext ctx;
	ctx.title = opennova::title_identity(opennova::gameprofile::GAME_JO);
	TEST_EXPECT(ctx.title != nullptr);
	ctx.gate_tag = ctx.title->gate_tag;
	ctx.startup_url = "http://gs.opennova.test:8080/NWStart.dll?prepare=1";
	ctx.post_ip = "gs.opennova.test";
	ctx.post_port = "8080";
	ctx.remember_login = false;
	opennova::LobbyHttpFlow flow;
	flow.share_browser(state);
	flow.set_context(ctx);
	TEST_EXPECT(&flow.browser() == &state);

	opennova::EpaskParams epask;
	TEST_EXPECT(opennova::generate_epask(epask));
	flow.login("player", "secret");
	flow.on_login_response(true, 200, {"Set-Cookie: EPASK=" + opennova::epask_to_string(epask)}, {});
	flow.on_login_response(true, 200, {"Set-Cookie: LOGINSESSIONTAG=t; Path=/"},
			page("<HTML><HEAD><META http-equiv=\"refresh\" CONTENT=0;URL=\"NWLogin.dll\"></HEAD></HTML>"));
	TEST_EXPECT(saved_at(path, key).empty());
	const opennova::LoginResult login = flow.on_login_response(true, 200,
			{"Set-Cookie: NWHANDLE=P; Path=/", "Set-Cookie: PERSISTENTEXPRESSLOGINDATA=E; Path=/"},
			page("<HTML></HTML>"));
	TEST_EXPECT(login.kind == opennova::LoginResult::Kind::Succeeded);
	CookieJar file = saved_at(path, key);
	TEST_EXPECT(file.entries().size() == 1 && *file.find("PERSISTENTEXPRESSLOGINDATA") == "E");
	TEST_EXPECT(state.jar.find("NWHANDLE") != nullptr);

	// The sign-in page's activation: REMEMBER_LOGIN takes rememberlogin (clear here).
	TEST_EXPECT(!state.remember_login_checked);
	flow.reshow_sign_in();
	TEST_EXPECT(state.remember_login_checked && !*state.remember_login_checked);

	const std::string nk = opennova::url_cipher_encode("10.1.2.3:3001", opennova::URL_CIPHER_KEY_NK);
	flow.join(777);
	flow.on_join_response(true, 200, {"Set-Cookie: PERSISTENTRELAY=1; Path=/"},
			page("<HTML><HEAD><META http-equiv=\"refresh\" CONTENT=0;URL=\"NWJoin.dll\"></HEAD></HTML>"));
	TEST_EXPECT(saved_at(path, key).find("PERSISTENTRELAY") == nullptr);
	const opennova::JoinResult joined = flow.on_join_response(true, 200,
			{"Set-Cookie: PERSISTENTJOIN=2; Path=/"},
			page("<TITLE> [NK=" + nk + "&NI=192.168.5.9&NP=17479&GS=x]</TITLE>"));
	TEST_EXPECT(joined.kind == opennova::JoinResult::Kind::Resolved);
	file = saved_at(path, key);
	TEST_EXPECT(file.find("PERSISTENTJOIN") != nullptr && file.find("PERSISTENTRELAY") != nullptr);
	TEST_EXPECT(file.find("PERSISTENTEXPRESSLOGINDATA") == nullptr);
	std::vector<uint8_t> bytes;
	std::string error;
	TEST_EXPECT(opennova::io::read_file_bytes(path, bytes, error) && bytes.size() > 8 && bytes[0] == 0);

	// The jar is the menu's: a session's reset leaves it whole.
	flow.reset();
	TEST_EXPECT(flow.cookies().find("NWHANDLE") != nullptr && flow.cookies().find("PERSISTENTJOIN") != nullptr);
	return 0;
}

// The sign-in page's REMEMBER_LOGIN checkbox clicked (D-NET-NEW-L9-62): its state after the click
// is the scene's property, which the next save reads (unchecked: the leading word 0 and no
// PERSISTENTEXPRESSLOGINDATA), and game.cfg's rememberlogin, the word returned, which the next
// activation's fill reads and sets the checkbox from.
// [orig: CCheckboxWnd_HandleNamedEvent @0x64AB80 - the flip @0x64ABE2..0x64ABF2, the property's
//  write called @0x64AC18; UI_StoreRememberLoginCheckbox @0x5574F0 - the store @0x55750D;
//  CUIScene_SaveCookieJarFile @0x63BA60 - the flag 0 only for "UNCHECKED" (_stricmp @0x63BA75);
//  UI_LoadPersistentLoginCredentials @0x557330 - the rememberLogin_4D4 test @0x557344, the checkbox
//  set (CCheckWnd_SetCheckedState called @0x55744A)]
int test_remember_login_click() {
	test_temp::TempDir dir("cookie_jar_remember");
	const std::string path = dir.file("nw_cdata.coo");
	const std::string key = opennova::cookie_jar_key(chosen_volume());
	opennova::MenuBrowserState state;
	state.create(path, key);
	std::vector<uint8_t> plain = {'A', 'C', 'C', 'T', 0, 'x', 0};
	const std::string remembered = opennova::encode_key_chain(plain, opennova::kRememberedLoginKeyChain);
	state.jar.update({entry("PERSISTENTREMEMBERLOGINDATA", remembered, std::string("/")),
			entry("PERSISTENTEXPRESSLOGINDATA", "E", std::string("/"))});
	opennova::LobbyHttpContext ctx;
	ctx.remember_login = true;
	opennova::LobbyHttpFlow flow;
	flow.share_browser(state);
	flow.set_context(ctx);

	// The activation with rememberlogin set: checked, the remembered login filled.
	flow.reshow_sign_in();
	TEST_EXPECT(state.remember_login_checked && *state.remember_login_checked);
	TEST_EXPECT(flow.has_remembered_login() && flow.remembered_login_name() == "ACCT");

	// A click clears it: rememberlogin 0; the save drops the express cookie under the word 0.
	TEST_EXPECT(flow.store_remember_login(false) == 0);
	TEST_EXPECT(state.remember_login_checked && !*state.remember_login_checked);
	TEST_EXPECT(!flow.context().remember_login);
	std::string error;
	TEST_EXPECT(state.save(error));
	std::vector<uint8_t> bytes;
	TEST_EXPECT(opennova::io::read_file_bytes(path, bytes, error));
	TEST_EXPECT(bytes == opennova::write_cookie_jar(state.jar, true, key) && bytes[0] == 0);
	CookieJar file = saved_at(path, key);
	TEST_EXPECT(file.entries().size() == 1 && file.find("PERSISTENTEXPRESSLOGINDATA") == nullptr);
	// The next activation reads it: unchecked, no fill.
	flow.reshow_sign_in();
	TEST_EXPECT(state.remember_login_checked && !*state.remember_login_checked);
	TEST_EXPECT(!flow.has_remembered_login() && flow.remembered_login_name().empty());

	// A click checks it again: rememberlogin 1, the express cookie saved under the word 1.
	TEST_EXPECT(flow.store_remember_login(true) == 1);
	TEST_EXPECT(state.remember_login_checked && *state.remember_login_checked);
	TEST_EXPECT(state.save(error));
	TEST_EXPECT(opennova::io::read_file_bytes(path, bytes, error) && bytes[0] == 1);
	file = saved_at(path, key);
	TEST_EXPECT(file.entries().size() == 2 && *file.find("PERSISTENTEXPRESSLOGINDATA") == "E");
	flow.reshow_sign_in();
	TEST_EXPECT(flow.has_remembered_login() && flow.remembered_login_name() == "ACCT");
	return 0;
}

// The response handler's status rules (D-NET-NEW-L9-63), which it applies to every answer: a 304
// stores no cookie, nor does a 301 or 302 whose header text holds "LOCATION" anywhere in any case
// (a Location header, a Content-Location one, the word inside a cookie's own text: the redirect is
// followed instead); a 301 or 302 with no such word, a 303 or 307 with one, a 200 with one, a 404
// and a 500 store theirs.
// [orig: HTTP_ResponseHandler @0x659480 - 301 tested @0x6594E2, 302 @0x6594EC, 304 @0x6594F3 (1
//  returned @0x65950A), "LOCATION" searched (String_FindCaseInsensitiveSubstring called @0x65951C;
//  none: the jz @0x659528 to the cookies), the SET-COOKIE scan @0x65961B]
int test_response_status_rules() {
	using opennova::response_cookie_batch;
	const std::string set = "Set-Cookie: A=1; Path=/";
	const auto stores = [&](int status, std::vector<std::string> headers) {
		const std::vector<CookieEntry> batch = response_cookie_batch(status, headers);
		return batch.size() == 1 && batch[0].name == "A" && batch[0].value == "1";
	};
	TEST_EXPECT(stores(200, {set}));
	TEST_EXPECT(!stores(304, {set}));
	TEST_EXPECT(!stores(304, {"Content-Type: text/html", set}));
	TEST_EXPECT(!stores(301, {"Location: http://gs.opennova.test/a", set}));
	TEST_EXPECT(!stores(302, {set, "Location: /NWStart.dll"}));
	TEST_EXPECT(!stores(302, {"location:/a", set}));
	TEST_EXPECT(!stores(302, {"Content-Location: /a", set}));
	TEST_EXPECT(!stores(302, {"Set-Cookie: A=1; Path=/LocationX"}));
	TEST_EXPECT(stores(301, {set}));
	TEST_EXPECT(stores(302, {"Content-Type: text/html", set}));
	TEST_EXPECT(stores(303, {"Location: /a", set}));
	TEST_EXPECT(stores(307, {"Location: /a", set}));
	TEST_EXPECT(stores(200, {"Location: /a", set}));
	TEST_EXPECT(stores(404, {set}));
	TEST_EXPECT(stores(500, {set}));
	return 0;
}

// The flow stores an answer's cookies ahead of its leg's status test, under the status rules: a
// failed login's 404 keeps its cookie, a 304's and a LOCATION 302's are dropped, a 302 with none
// keeps its own.
// [orig: CGopherWnd_PumpRequests @0x659740 - HTTP_ResponseHandler called @0x6599F3]
int test_flow_merges_every_answer() {
	opennova::LobbyHttpContext ctx;
	ctx.title = opennova::title_identity(opennova::gameprofile::GAME_JO);
	TEST_EXPECT(ctx.title != nullptr);
	ctx.gate_tag = ctx.title->gate_tag;
	ctx.startup_url = "http://gs.opennova.test:8080/NWStart.dll?prepare=1";
	ctx.post_ip = "gs.opennova.test";
	ctx.post_port = "8080";
	opennova::LobbyHttpFlow flow;
	flow.set_context(ctx);
	const auto answer = [&](int status, std::vector<std::string> headers) {
		TEST_EXPECT(flow.login("player", "secret").kind == opennova::LoginResult::Kind::NeedRequest);
		TEST_EXPECT(flow.on_login_response(true, status, headers, page("<HTML></HTML>")).kind ==
		            opennova::LoginResult::Kind::Failed);
		return 0;
	};
	TEST_EXPECT(answer(404, {"Set-Cookie: FAILED=1; Path=/"}) == 0);
	TEST_EXPECT(answer(304, {"Set-Cookie: UNMODIFIED=1; Path=/"}) == 0);
	TEST_EXPECT(answer(302, {"Location: /a", "Set-Cookie: MOVED=1; Path=/"}) == 0);
	TEST_EXPECT(answer(302, {"Set-Cookie: STAYED=1; Path=/"}) == 0);
	TEST_EXPECT(flow.cookies().find("FAILED") != nullptr && *flow.cookies().find("FAILED") == "1");
	TEST_EXPECT(flow.cookies().find("UNMODIFIED") == nullptr && flow.cookies().find("MOVED") == nullptr);
	TEST_EXPECT(flow.cookies().find("STAYED") != nullptr);
	return 0;
}

} // namespace

int main() {
	if (test_parse_path() != 0) return 1;
	if (test_parse_walks_into_the_next_line() != 0) return 1;
	if (test_parse_expires_and_secure() != 0) return 1;
	if (test_store() != 0) return 1;
	if (test_volume_key() != 0) return 1;
	if (test_writer() != 0) return 1;
	if (test_reader() != 0) return 1;
	if (test_menu_browser_state() != 0) return 1;
	if (test_flow_save_sites() != 0) return 1;
	if (test_remember_login_click() != 0) return 1;
	if (test_response_status_rules() != 0) return 1;
	if (test_flow_merges_every_answer() != 0) return 1;
	std::printf("cookie_jar: all checks passed\n");
	return 0;
}
