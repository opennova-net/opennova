// The service's two-call NWJoin.dll / NWHost.dll relay leg selection
// (net/novaworld/relay_request.h). A stock client sends its whole persistent
// cookie jar on every GET [orig: CUIBrowser_SendHTTPRequest @0x658a44..0x658ac0],
// so the NWJOINSESSIONTAG a previous join or host left behind rides every later
// first call; that call must still start a fresh relay instead of resolving the
// stale (possibly evicted) tag into an HTTP 400.

#include <net/novaworld/relay_request.h>

#include <cstdio>

namespace {

int g_failures = 0;

void check(bool cond, const char *what) {
	if (!cond) {
		std::printf("  FAIL: %s\n", what);
		++g_failures;
	}
}

} // namespace

int main() {
	using opennova::classify_relay_request;
	using opennova::RelayLeg;

	// A first join: the page link's query, no cookie yet.
	check(classify_relay_request(false, true, false) == RelayLeg::First,
	      "a first call with no cookie starts a relay");
	// A second join (or a host after a join): the page link's query plus the
	// previous relay's tag cookie. Before, this resolved the stale tag.
	check(classify_relay_request(false, true, true) == RelayLeg::First,
	      "a first-call query starts a fresh relay despite a stale tag cookie");
	// The stock relay page's bare refresh: the cookie carries the tag.
	check(classify_relay_request(false, false, true) == RelayLeg::Second,
	      "a bare refresh resolves the cookie's tag");
	// Our client's follow-up carries the tag in the query (with rid=).
	check(classify_relay_request(true, true, true) == RelayLeg::Second,
	      "an explicit tag query is the second call");
	check(classify_relay_request(true, false, false) == RelayLeg::Second,
	      "an explicit tag query alone is the second call");
	// Nothing at all: a fresh relay.
	check(classify_relay_request(false, false, false) == RelayLeg::First,
	      "a bare request with no cookie starts a relay");

	if (g_failures == 0) {
		std::printf("relay_request: all checks passed\n");
		return 0;
	}
	std::printf("relay_request: %d check(s) failed\n", g_failures);
	return 1;
}
