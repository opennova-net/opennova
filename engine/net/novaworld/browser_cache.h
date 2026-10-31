#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

// The menu browser's content cache, as the NovaWorld web flow (LobbyHttpFlow) meets it: the
// URL the browser combines a page's relative reference into, the cache key it files a download
// under, and the process's one list of those keys. A page's IB3_TEMPLATE (.mnx) goes through it,
// so the first fetch of a template in a process downloads it and every later one is served from
// the browser's disk copy with no request (no expiry, no conditional GET).
namespace opennova {

// A relative reference combined with the URL of the page that named it, as the menu browser
// combines them: a reference with a scheme is taken whole; otherwise the base's scheme, "://",
// then the reference's host (with ":" and the reference's port when the BASE has a host and a
// port) or the base's host and port, then a relative path after "/" with the reference's params
// and query, or else the base's path with the reference's params and query or the base's. The
// fragment is never kept. A relative path replaces the base's whole path: nothing of the base's
// directory is kept.
// [orig: URL_CombineComponents @0x650BC0; URL_ParseComponents @0x650140]
std::string browser_combine_url(const std::string &base, const std::string &relative);

// The cache key a fetch of `url` (already combined) is filed under: "cache\" and the URL up to
// its first '?', each byte the CRT isalnum of the sign-extended byte (cp1252's) refuses, '.'
// apart, as '_'.
// [orig: CUIBrowser_AsyncFetchURLContent @0x64DE80 - the combine called @0x64DE9B, the '?' scan
//  @0x64DEA7..0x64DEBD, isalnum called @0x64DEF4 over the movsx byte, '.' @0x64DF00, '_'
//  @0x64DF05, "cache\" (0x7E14F0) copied @0x64DF28..0x64DF44]
std::string browser_cache_key(const std::string &url);

// The browser's cache list as a page's template meets it: one entry per key, keys compared
// case-insensitively. A fetch whose key has no entry files one and requests the URL; a fetch
// whose entry holds a download is served from it; a fetch whose entry's download is in flight
// joins it. The template's fetch passes the store-and-keep flag (the fetch's user_context, the
// entry's +0x24), so a download that ends with a stored body (a 2xx with bytes; the browser writes
// the file only then) is held for the process's life; any other end leaves no file, so the next
// fetch of the key requests it again. A page's own fetch passes the flag clear: its entry is
// unlinked at its end and never stored, so pages are not filed here.
// [orig: CUIBrowser_AsyncFetchURLContent @0x64DE80 - the walk over g_UICacheEntryList
//  (String_CompareNoCase called @0x64DFF9), a new entry and its request (HTTP_RequestEnqueue
//  called @0x64E114), the held test `cmp [esi+0Ch], ebx` @0x64E122, the cache file's fopen
//  called @0x64E131 (none: the entry dropped and filed afresh @0x64E13F..0x64E18F), the served
//  callback @0x64E1FB, a pending entry's callback appended and no second request
//  (@0x64E2D8..0x64E2DE); UI_ProcessWebResponseContent @0x63D450 - the template's user_context 1
//  pushed @0x63D6C5; UIScene_OpenUrl @0x63E2F0 - the page's user_context 0 pushed @0x63E39A;
//  HTTP_DownloadCompletionCallback @0x64DB20 - the 2xx window @0x64DB8D..0x64DBB3, the write's
//  tests @0x64DBC1 / @0x64DBCA and the store flag's (+0x24) @0x64DBD3, its fopen called
//  @0x64DBED, the entry's +0xC set @0x64DC9D on every end, an entry whose store flag is clear
//  unlinked and freed @0x64DCFB..0x64DD52]
class BrowserCache {
public:
	// A fetch of `url`: true when it goes out as a request (the key has no entry, or its entry
	// holds no stored download and none is in flight), false when the entry serves it.
	bool fetch(const std::string &url);
	// The end of a requested fetch of `url`: `stored` the body the browser wrote (a 2xx answer
	// with bytes), none when it wrote none.
	void complete(const std::string &url, std::optional<std::vector<uint8_t>> stored);
	// Whether the key of `url` holds a stored download.
	bool holds(const std::string &url) const;
	// The stored download of `url`'s key, the bytes its cache file holds, as the browser reads a
	// served template back (null when the key holds none).
	const std::vector<uint8_t> *stored(const std::string &url) const;
	// Every entry dropped: the menu's teardown frees the list (browser_menu_destroyed).
	// [orig: CUIScene_DestroyAllContent @0x63C490 - CUICache_PruneFileCache called @0x63C4A2,
	//  which frees the list (CUICache_FreeEntryList called @0x64DA90)]
	void clear() { entries_.clear(); }

private:
	struct Entry {
		bool held = false; // the download is stored (else in flight)
		std::vector<uint8_t> body; // the stored download's bytes
	};
	std::map<std::string, Entry> entries_; // keyed by the lower-cased cache key
};

// The process's one cache list: the menu's, filed from its creation to its teardown, which
// outlives every NovaWorld entry and session of the process.
// [orig: g_UICacheEntryList (0x31C1528); CUIManager_Create @0x64AF10 - CUICache_ScanCacheDirectory
//  called @0x64AF68, once (Menu_InitShellResources @0x552500 creates g_GameMenu only while it is
//  null, @0x5525A6 / @0x5525D8)]
BrowserCache &process_browser_cache();

} // namespace opennova
