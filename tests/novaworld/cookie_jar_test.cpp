// The menu browser's cookie jar as retail keeps it (D-NET-NEW-L9-58): the Set-Cookie parse off the
// answer's header text, the store's match (the name case-insensitively, then the path rule) and the
// by-name read.
// [orig: HTTP_ResponseHandler @0x659480; HttpCookie_Parse @0x658F60; CookieJar_UpdateFromURL
//  @0x64E630; CookieJar_GetValueByName @0x64EA90; String_CompareNoCase @0x650000]

#include <net/novaworld/http_login.h>

#include "common/test_expect.h"

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

} // namespace

int main() {
	if (test_parse_path() != 0) return 1;
	if (test_parse_walks_into_the_next_line() != 0) return 1;
	if (test_parse_expires_and_secure() != 0) return 1;
	if (test_store() != 0) return 1;
	std::printf("cookie_jar: all checks passed\n");
	return 0;
}
