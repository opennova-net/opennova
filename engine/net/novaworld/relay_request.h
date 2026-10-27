#pragma once

#include <cstdint>

namespace opennova {

// The two-call NWJoin.dll / NWHost.dll relay our service runs: the first call
// (the page link, carrying its query: rid, success, relay, mode, ...) mints a
// session tag and answers the relay page; the relay's bare refresh resolves that
// tag from its cookie. A stock client sends its whole cookie jar on every GET,
// and the jar persists across joins and hosts [orig: CUIBrowser_SendHTTPRequest
// @0x658a44..0x658ac0; the nw_cdata.coo save @0x54f2c0], so a stale
// NWJOINSESSIONTAG cookie rides every later first call. Service policy
// (NovaLogic's service has no witness): a first-call query starts a fresh relay
// whatever the cookie says, and only a bare refresh falls back to the cookie's
// tag. No client sends the tag in a query (our flow follows the relay page's
// bare refresh as the menu browser does, D-NET-1251), so none is read.
enum class RelayLeg : uint8_t {
	First,
	Second,
};

inline RelayLeg classify_relay_request(bool has_first_call_query, bool has_tag_cookie) {
	if (has_first_call_query) return RelayLeg::First;
	return has_tag_cookie ? RelayLeg::Second : RelayLeg::First;
}

} // namespace opennova
