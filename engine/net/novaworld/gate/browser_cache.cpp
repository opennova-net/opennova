#include <net/novaworld/browser_cache.h>

#include <cstring>
#include <optional>

#include <base/io/cp1252.h>
#include <base/io/strutil.h>

namespace opennova {
namespace {

// A URL's parts as the menu browser splits them. Every part keeps its delimiter but the scheme's
// and the host's: the port is the text past ':', the query starts at '?', the params at ';', the
// fragment at '#'. The path is always set (empty when nothing is left); `relative` when it does
// not start with '/'.
struct UrlParts {
	std::optional<std::string> scheme, host, port, path, params, query, fragment;
	bool relative = false;
};

// [orig: URL_ParseComponents @0x650140 - the fragment cut at '#' @0x6501A7..0x6501EC, the scheme
//  before the first ':' when strspn over the scheme set covers it @0x6501F8..0x650251, "//"
//  @0x650269: a non-file host to '/' or ':' @0x650322..0x650367 and its port to '/'
//  @0x650393..0x6503D4, a file URL's host before the last '/' ahead of a '?' or the first '/'
//  @0x65029C..0x65031D; the query cut at '?' @0x6503D8..0x650420, the params at ';'
//  @0x650426..0x65046C, the path absolute at a '/' @0x650477, else relative @0x650479]
UrlParts parse_url(const std::string &url) {
	UrlParts parts;
	std::string work = url;
	if (const std::size_t hash = work.find('#'); hash != std::string::npos) {
		parts.fragment = work.substr(hash);
		work.resize(hash);
	}
	if (const std::size_t colon = work.find(':'); colon != std::string::npos) {
		const std::string scheme = work.substr(0, colon);
		if (scheme.find_first_not_of(
					"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ+-.") == std::string::npos) {
			parts.scheme = scheme;
			work.erase(0, colon + 1);
		}
	}
	if (work.size() >= 2 && work[0] == '/' && work[1] == '/') {
		if (!parts.scheme || !strutil::iequals(*parts.scheme, "file")) {
			std::size_t scan = 2;
			while (scan < work.size() && work[scan] != '/' && work[scan] != ':') ++scan;
			parts.host = work.substr(2, scan - 2);
			if (scan < work.size() && work[scan] == ':') {
				std::size_t end = scan;
				while (end < work.size() && work[end] != '/') ++end;
				parts.port = work.substr(scan + 1, end - scan - 1);
				work.erase(0, end);
			} else {
				work.erase(0, scan);
			}
		} else {
			// A file URL: the host is what precedes the last '/' ahead of a '?', else the first
			// '/', unless that '/' is the authority's own.
			const std::size_t question = work.find('?');
			std::size_t slash = std::string::npos;
			bool at_start = true;
			if (question != std::string::npos) {
				slash = question;
				while (work[slash] != '/') --slash;
				at_start = slash == 1;
			} else {
				slash = work.find('/');
				at_start = slash == 0;
			}
			if (!at_start) parts.host = work.substr(2, slash - 2);
			work.erase(0, 2 + (parts.host ? parts.host->size() : 0));
		}
	}
	if (const std::size_t question = work.find('?'); question != std::string::npos) {
		parts.query = work.substr(question);
		work.resize(question);
	}
	if (const std::size_t semicolon = work.find(';'); semicolon != std::string::npos) {
		parts.params = work.substr(semicolon);
		work.resize(semicolon);
	}
	parts.relative = work.empty() || work[0] != '/';
	parts.path = work;
	return parts;
}

} // namespace

// See browser_cache.h.
// [orig: URL_CombineComponents @0x650BC0 - a reference with a scheme taken whole @0x650C33 /
//  @0x650D5B; the base's scheme and "://" @0x650DD8..0x650E06; a reference's host
//  @0x65143D..0x651477, ":" when the base has a port and a host @0x65149F..0x6514F4 and then the
//  reference's port @0x65150A..0x65157F, "/" @0x6515E3, the reference's path, params and query
//  @0x6515F7..0x65175E; else the base's host @0x650E27..0x650E95 and ":" with its port
//  @0x650EBC..0x650F87; a relative path after "/" (only with a base host) with the reference's
//  params and query @0x650FB2..0x651175; otherwise the base's path @0x6511A5..0x651211, the
//  reference's params or the base's @0x65122F..0x65130F, the reference's query or the base's
//  @0x65132D..0x65175E]
std::string browser_combine_url(const std::string &base, const std::string &relative) {
	const UrlParts rel = parse_url(relative);
	if (rel.scheme) return relative;
	const UrlParts b = parse_url(base);
	std::string out = b.scheme.value_or(std::string()) + "://";
	if (rel.host) {
		out += *rel.host;
		if (b.port && b.host) {
			out += ":";
			if (rel.port) out += *rel.port;
		}
		out += "/";
		out += *rel.path;
		if (rel.params) out += *rel.params;
		if (rel.query) out += *rel.query;
		return out;
	}
	if (b.host) out += *b.host;
	if (b.port && b.host) out += ":" + *b.port;
	if (rel.relative) {
		if (b.host) out += "/";
		out += *rel.path;
		if (rel.params) out += *rel.params;
		if (rel.query) out += *rel.query;
		return out;
	}
	if (b.path) out += *b.path;
	if (rel.params) out += *rel.params;
	else if (b.params) out += *b.params;
	if (rel.query) out += *rel.query;
	else if (b.query) out += *b.query;
	return out;
}

// See browser_cache.h.
std::string browser_cache_key(const std::string &url) {
	std::string name = url.substr(0, url.find('?'));
	for (char &c : name) {
		const bool alnum = cp1252_isalpha_sign_extended(c) || cp1252_isdigit(static_cast<uint8_t>(c));
		if (!alnum && c != '.') c = '_';
	}
	return "cache\\" + name;
}

namespace {
// The list's compare as a map key: every byte lowered as the CRT stricmp under the game's
// ".ACP" locale lowers it (cp1252_stricmp's fold), so two keys that compare equal map alike.
// [orig: String_CompareNoCase @0x650000 - the tail jump to _stricmp @0x650026]
std::string entry_key(const std::string &url) {
	std::string key = browser_cache_key(url);
	for (char &c : key) c = static_cast<char>(cp1252_tolower(static_cast<uint8_t>(c)));
	return key;
}
} // namespace

bool BrowserCache::fetch(const std::string &url) {
	const auto [it, filed] = entries_.emplace(entry_key(url), Entry::Pending);
	(void)it;
	return filed;
}

void BrowserCache::complete(const std::string &url, bool stored) {
	const auto it = entries_.find(entry_key(url));
	if (it == entries_.end()) return;
	if (stored)
		it->second = Entry::Held;
	else
		entries_.erase(it);
}

bool BrowserCache::holds(const std::string &url) const {
	const auto it = entries_.find(entry_key(url));
	return it != entries_.end() && it->second == Entry::Held;
}

BrowserCache &process_browser_cache() {
	static BrowserCache cache;
	return cache;
}

} // namespace opennova
