#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <net/http/http_exchange.h>
#include <net/novaworld/browser_cache.h> // BrowserCache, process_browser_cache
#include <net/novaworld/http_flow.h> // HttpRequestSpec, browser_cache_bust_url, browser_request_host

// The NovaWorld menu browser's HTTP transport: what carries the web flow's requests
// (LobbyHttpFlow's HttpRequestSpec) for the game's NovaWorld client and opennova-serve's lister
// alike, as retail's CGopherWnd_PumpRequests carries the menu's (ADR 0077 d2: the game's HTTP is
// the engine's, over the platform's sockets). One request resolves its server once, through the
// browser's name list, writes the nocachefix pair over that address on a GET (D-NET-1251), leads
// its lines with the port-less Host (D-NET-1329), connects to the address at the URL's port and
// sends the request HTTP/1.0 as the browser writes it (net::http_exchange), then reads the reply
// in the browser's framing (net::HttpFraming::Browser). The connect waits 60 s; the reply's
// bytes wait without a bound; a 301 / 302 is handed over as it came (the flow's cookie rule,
// D-NET-1352, D-NET-1355).
namespace opennova {

// The menu browser's connect wait: 60 s from the connect's start.
// [orig: CNapiWinsock_ConnectNonBlocking @0x667600 - the start's GetTickCount stored in
//  g_WinsockConnectStartTick @0x6676D9 at the connect's WSAEWOULDBLOCK, the 10 ms select poll
//  @0x667725, `GetTickCount() - start < 0xEA60` @0x6677D6, the close and E_FAIL past it
//  @0x66780F / @0x667820; HTTP_RecvResponse @0x666EF0 - a non-blocking recv each pump, no bound]
inline constexpr int kBrowserConnectTimeoutMs = 60000;

// The browser's name list: every name a lookup resolved and its first address, newest first, the
// names compared case-insensitively; an entry whose address is 0.0.0.0 reads as none. The
// process keeps one (process_browser_names), from the menu's creation to its teardown.
// [orig: CUINameValueList_FindByName @0x64FF00 - the walk over g_UINameValueList, _stricmp called
//  @0x64FF15, the value 0 for none @0x64FF29; CUINameValueList_Add @0x64FF90 - the node
//  prepended @0x64FFEC..0x64FFEF; CUINameValueList_FreeAll @0x64FF40, called from
//  CUICache_ScanCacheDirectory @0x64D6C5 (CUIManager_Create's, once) and CUICache_PruneFileCache
//  @0x64D918 (the menu's teardown, CUIScene_DestroyAllContent)]
class BrowserNames {
public:
	std::optional<std::array<uint8_t, 4>> find(const std::string &name) const;
	void add(const std::string &name, const std::array<uint8_t, 4> &ip);
	void clear();

private:
	mutable std::mutex mutex_;
	std::vector<std::pair<std::string, std::array<uint8_t, 4>>> entries_; // newest first
};

BrowserNames &process_browser_names();

// The menu's creation as the browser's two process lists meet it: the name list freed, so the next
// request's server is looked up afresh; then the scan of the cache directory, which files an entry
// per file there that holds nothing until its next download ends (a fetch of its key requests it
// and takes its callback, as one of a key with no entry does), so the cache serves nothing more
// than it did (BrowserCache: a key with no entry). Ours runs at each of the menu's creations: the
// first main-menu entry and the one after a mods reload.
// [orig: Menu_InitShellResources @0x552500 - CUIManager_Create called @0x5525D8 while g_GameMenu is
//  null; CUIManager_Create @0x64AF10 - CUICache_ScanCacheDirectory called @0x64AF68;
//  CUICache_ScanCacheDirectory @0x64D6B0 - CUINameValueList_FreeAll called @0x64D6C5, each file's
//  entry prepended with +0xC 0 @0x64D7DD..0x64D7ED; CUIBrowser_AsyncFetchURLContent @0x64DE80 - an
//  entry whose +0xC is 0 takes the fetch's callback (@0x64E122) and its first requests the URL
//  (HTTP_RequestEnqueue called @0x64E303)]
void browser_menu_created(BrowserNames &names = process_browser_names());

// The menu's teardown that destroys it (the way out, a mods reload ahead of the re-creation): the
// name list freed, then the cache list (BrowserCache::clear: a download still in flight is dropped,
// and the next fetch of any key requests it, as the re-creation's scanned entries do).
// [orig: Menu_TeardownShellAndCloseBinkVideos @0x54E430 - under g_MenuDestroyOnTeardown (@0x54E4A3)
//  the menu destroyed, sub_64AFE0 called @0x54E4BA; CGameMenu_Destructor @0x63E100 -
//  CUIScene_DestroyAllContent called @0x63E137; CUIScene_DestroyAllContent @0x63C490 -
//  CUICache_PruneFileCache called @0x63C4A2; CUICache_PruneFileCache @0x64D900 -
//  CUINameValueList_FreeAll called @0x64D918, CUICache_FreeEntryList called @0x64DA90]
void browser_menu_destroyed(BrowserNames &names = process_browser_names(),
		BrowserCache &cache = process_browser_cache());

// The platform's name lookup: the first IPv4 address `host` resolves to (net::resolve_ipv4);
// serve's applies its destination rule first. False when it does not resolve.
using BrowserLookup = std::function<bool(const std::string &host, std::array<uint8_t, 4> &ip)>;

// A request's server as the browser resolves it: a dotted quad as it reads, else the name list's
// address, else the lookup's first, which the list then keeps. False when none resolves.
// [orig: CNapiWinsock_ResolveHostAsync @0x667460 - inet_addr @0x66747E (a dotted quad: 0
//  returned @0x66748E), CUINameValueList_FindByName called @0x667495 (found: 0 @0x6675F9), else
//  the lookup's thread created @0x667550; CNapiWinsock_ResolveHostThread @0x666C70 -
//  gethostbyname @0x666C79, its first address @0x666C8A, CUINameValueList_Add called @0x666C92]
bool browser_resolve(const std::string &host, const BrowserLookup &lookup,
		std::array<uint8_t, 4> &ip, BrowserNames &names = process_browser_names());

struct BrowserTransportOptions {
	BrowserLookup lookup;              // the platform's lookup (net::resolve_ipv4 over names)
	uint32_t (*tick_ms)() = nullptr;   // the process's GetTickCount (a GET's nocachefix tick)
	int connect_ms = kBrowserConnectTimeoutMs;
	int io_ms = 0;                     // >0: a wait for the reply's bytes this long ends it (serve)
	size_t max_reply_bytes = net::kHttpMaxReplyBytes;
};

// The URL a menu-browser request writes, once its server resolved to `server_ip`: the fragment
// cut (the browser's URL parse keeps it apart and never sends it), then on a GET the nocachefix
// pair over `tick_ms` and that address (browser_cache_bust_url).
// [orig: URL_ParseComponents @0x650140 - the fragment cut first (strchr '#' called @0x6501A7);
//  CUIBrowser_SendHTTPRequest @0x658840 - the path, the query and the pair written @0x6588DC]
std::string browser_request_url(const HttpRequestSpec &spec, uint32_t tick_ms,
		const std::array<uint8_t, 4> &server_ip);

// One menu-browser request, on the calling thread: the server resolved once (browser_resolve over
// the URL's host, browser_request_host's name), the request written as browser_request_url and
// the Host line make it, the connect to that address. `cancelled`, set from another thread, ends
// it at its next wait for bytes.
// [orig: CGopherWnd_PumpRequests @0x659740 - CNapiWinsock_InitFromUrl called @0x659797 over the
//  request's +0x28 (the address into its +0x30), CNapiWinsock_ConnectNonBlocking called @0x6597F7
//  at `lea eax, [esi+30h]` @0x6597F3, CUIBrowser_SendHTTPRequest called @0x6598EA (a GET) or
//  GopherWebWidget_SendHttpPost @0x65992D (a form post), HTTP_RecvResponse called @0x65996D,
//  HTTP_ResponseHandler called @0x6599F3]
net::HttpReply browser_exchange(const HttpRequestSpec &spec, const BrowserTransportOptions &options,
		net::IStreamConnector &connector = net::tcp_stream_connector(),
		const std::atomic<bool> *cancelled = nullptr);

// One request at a time off the caller's thread: ship() runs browser_exchange on a worker of its
// own (a request still running is cancelled and never reports), take() hands its reply over once,
// on the caller's thread, when it has finished. cancel() and the destructor never wait: a worker
// still in its lookup or its connect ends on its own, its reply dropped.
class BrowserChannel {
public:
	explicit BrowserChannel(BrowserTransportOptions options);
	~BrowserChannel();
	BrowserChannel(const BrowserChannel &) = delete;
	BrowserChannel &operator=(const BrowserChannel &) = delete;

	void ship(const HttpRequestSpec &spec);
	// The running request ends unreported.
	void cancel();
	// A request is out (running, or finished and not yet taken).
	bool busy() const { return job_ != nullptr; }
	// The finished request's reply, once; none while it runs or when none is out.
	std::optional<net::HttpReply> take();

private:
	struct Job;
	BrowserTransportOptions options_;
	std::shared_ptr<Job> job_;
};

} // namespace opennova
