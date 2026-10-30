#include <net/novaworld/browser_transport.h>

#include <base/io/strutil.h>

#include <thread>

namespace opennova {

// The newest entry of the name decides: its address, or none when it is 0.0.0.0.
// [orig: CUINameValueList_FindByName @0x64FF00 - the first match's value returned @0x64FF2D]
std::optional<std::array<uint8_t, 4>> BrowserNames::find(const std::string &name) const {
	const std::lock_guard<std::mutex> lock(mutex_);
	for (const auto &entry : entries_) {
		if (!strutil::iequals(entry.first, name)) continue;
		if (entry.second == std::array<uint8_t, 4>{}) return std::nullopt;
		return entry.second;
	}
	return std::nullopt;
}

void BrowserNames::add(const std::string &name, const std::array<uint8_t, 4> &ip) {
	const std::lock_guard<std::mutex> lock(mutex_);
	entries_.insert(entries_.begin(), {name, ip});
}

void BrowserNames::clear() {
	const std::lock_guard<std::mutex> lock(mutex_);
	entries_.clear();
}

BrowserNames &process_browser_names() {
	// Never destroyed: a worker still in its lookup at the process's exit may yet add to it.
	static BrowserNames *names = new BrowserNames();
	return *names;
}

// [orig: CNapiWinsock_ResolveHostAsync @0x667460 - inet_addr @0x66747E, the list @0x667495, the
//  lookup's thread @0x667550; CNapiWinsock_ResolveHostThread @0x666C70 - the list's add @0x666C92]
bool browser_resolve(const std::string &host, const BrowserLookup &lookup,
		std::array<uint8_t, 4> &ip, BrowserNames &names) {
	net::Endpoint dotted;
	if (net::resolve_ipv4(host, dotted, /*allow_names=*/false)) {
		ip = dotted.ip;
		return true;
	}
	if (const std::optional<std::array<uint8_t, 4>> listed = names.find(host)) {
		ip = *listed;
		return true;
	}
	if (!lookup || !lookup(host, ip)) return false;
	names.add(host, ip);
	return true;
}

std::string browser_request_url(const HttpRequestSpec &spec, uint32_t tick_ms,
		const std::array<uint8_t, 4> &server_ip) {
	const std::string url = spec.url.substr(0, spec.url.find('#'));
	return spec.cache_bust ? browser_cache_bust_url(url, tick_ms, server_ip) : url;
}

net::HttpReply browser_exchange(const HttpRequestSpec &spec, const BrowserTransportOptions &options,
		net::IStreamConnector &connector, const std::atomic<bool> *cancelled) {
	net::HttpReply reply;
	if (!spec.valid) {
		reply.error = "no request";
		return reply;
	}
	// One resolve per request: the connect reaches the address the nocachefix pair reads.
	// [orig: CNapiWinsock_InitFromUrl @0x658DA0 - CNapiWinsock_ResolveHostAsync called @0x658E4B
	//  into the request's +0x30 word, which CUIBrowser_SendHTTPRequest @0x658840 reads for the
	//  pair @0x6588A3 and the connect reads @0x6597F3]
	const std::string host = browser_request_host(spec.url);
	std::array<uint8_t, 4> server{};
	if (!browser_resolve(host, options.lookup, server)) {
		reply.error = "cannot reach " + host;
		return reply;
	}
	const std::string url =
			browser_request_url(spec, options.tick_ms != nullptr ? options.tick_ms() : 0u, server);
	// The menu browser's Host line leads the flow's lines: the URL's host with no port
	// (browser_request_host, D-NET-1329), where the transport would write the authority.
	std::vector<std::string> headers{"Host: " + host};
	headers.insert(headers.end(), spec.headers.begin(), spec.headers.end());
	net::HttpExchangeOptions exchange;
	exchange.connect_ms = options.connect_ms;
	exchange.io_ms = options.io_ms;
	exchange.max_reply_bytes = options.max_reply_bytes;
	exchange.framing = net::HttpFraming::Browser;
	exchange.cancelled = cancelled;
	return net::http_exchange(connector, spec.method == HttpMethod::Post ? "POST" : "GET", url,
			headers, spec.body,
			[&server](const std::string &, net::Endpoint &out) {
				out.ip = server;
				return true;
			},
			exchange);
}

struct BrowserChannel::Job {
	std::atomic<bool> cancelled{false};
	std::atomic<bool> done{false};
	net::HttpReply reply; // written by the worker before `done`, read by the owner after it
};

// The platform's sockets are up from the first channel on (idempotent: a program that started
// them, or a backend that did, keeps its own).
BrowserChannel::BrowserChannel(BrowserTransportOptions options) : options_(std::move(options)) {
	net::startup();
}

BrowserChannel::~BrowserChannel() { cancel(); }

void BrowserChannel::ship(const HttpRequestSpec &spec) {
	cancel();
	std::shared_ptr<Job> job = std::make_shared<Job>();
	job_ = job;
	// The worker holds its job and its own copies, nothing of the channel's: a cancelled one runs
	// on to its next wait (or the end of its lookup or connect) and its reply goes nowhere.
	std::thread([job, spec, options = options_]() {
		net::HttpReply reply = browser_exchange(spec, options, net::tcp_stream_connector(),
				&job->cancelled);
		job->reply = std::move(reply);
		job->done.store(true, std::memory_order_release);
	}).detach();
}

void BrowserChannel::cancel() {
	if (job_ == nullptr) return;
	job_->cancelled.store(true);
	job_.reset();
}

std::optional<net::HttpReply> BrowserChannel::take() {
	if (job_ == nullptr || !job_->done.load(std::memory_order_acquire)) return std::nullopt;
	std::optional<net::HttpReply> reply(std::move(job_->reply));
	job_.reset();
	return reply;
}

} // namespace opennova
