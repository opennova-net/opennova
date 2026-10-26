#include "authoring/content_http.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <optional>
#include <string>
#include <system_error>
#include <utility>

#include <base/io/os_path.h>
#include <base/io/sha256.h>
#include <base/io/strutil.h>

#include <godot_cpp/classes/http_client.hpp>
#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/classes/tls_options.hpp>
#include <godot_cpp/classes/x509_certificate.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>

#include "util/string_convert.h"

namespace godot {

namespace {

using opennova::editor::ContentRequest;
using opennova::editor::ContentResponse;

// What a read asks of the connection at a time, and how long a worker waits on a connection with
// nothing for it.
constexpr int kChunkBytes = 256 * 1024;
constexpr std::chrono::milliseconds kWait(1);
// A connection left open is used again only this soon after its last reply, inside the service's
// own keep-alive window (Crow's, 5 s) and the store's: an older one the far end may be closing as
// the next request goes out.
constexpr int64_t kKeepMs = 4000;
// An upload's sending is given kStalledMs plus a millisecond for each kSlowestBytesPerMs bytes of
// its body: HTTPClient says nothing of how much of a request has gone.
constexpr int64_t kSlowestBytesPerMs = 16;
// How many more tries a request that may go again gets once its connection breaks.
constexpr int kRetries = 3;

int64_t now_ms() {
	return std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::steady_clock::now().time_since_epoch())
			.count();
}

// A URL the transport can reach: http or https, a host and a port, and the path and query it asks.
struct Address {
	bool tls = false;
	std::string host;
	int port = 0;
	std::string target;

	std::string origin() const {
		return (tls ? "https://" : "http://") + host + ":" + std::to_string(port);
	}
};

bool split_url(const std::string &url, Address &out) {
	std::string rest;
	if (url.rfind("https://", 0) == 0) {
		out.tls = true;
		out.port = 443;
		rest = url.substr(8);
	} else if (url.rfind("http://", 0) == 0) {
		out.tls = false;
		out.port = 80;
		rest = url.substr(7);
	} else {
		return false;
	}
	const size_t end = rest.find_first_of("/?#");
	const std::string authority = rest.substr(0, end);
	out.target = end == std::string::npos ? "/" : rest.substr(end);
	if (out.target[0] != '/')
		out.target = "/" + out.target;
	const size_t hash = out.target.find('#');
	if (hash != std::string::npos)
		out.target.resize(hash);
	if (authority.find_first_of("@[]") != std::string::npos)
		return false;
	const size_t colon = authority.find(':');
	out.host = authority.substr(0, colon);
	if (colon != std::string::npos) {
		const std::optional<int> port = opennova::strutil::parse_int(authority.substr(colon + 1));
		if (!port || *port < 1 || *port > 65535)
			return false;
		out.port = *port;
	}
	return !out.host.empty();
}

bool method_of(const std::string &name, HTTPClient::Method &out) {
	static const std::pair<const char *, HTTPClient::Method> kMethods[] = {
		{ "GET", HTTPClient::METHOD_GET },
		{ "POST", HTTPClient::METHOD_POST },
		{ "PUT", HTTPClient::METHOD_PUT },
		{ "PATCH", HTTPClient::METHOD_PATCH },
		{ "DELETE", HTTPClient::METHOD_DELETE },
	};
	for (const auto &[text, method] : kMethods) {
		if (name == text) {
			out = method;
			return true;
		}
	}
	return false;
}

// RFC 9110 9.2.2: a request a client may send again when a connection fails under it.
bool idempotent(HTTPClient::Method method) {
	return method == HTTPClient::METHOD_GET || method == HTTPClient::METHOD_PUT ||
			method == HTTPClient::METHOD_DELETE;
}

bool read_whole(const std::string &path, PackedByteArray &out, std::string &error) {
	std::FILE *file = opennova::io::fopen_utf8(path.c_str(), "rb");
	if (file == nullptr) {
		error = "cannot read " + path;
		return false;
	}
	bool ok = opennova::io::fseek64(file, 0, SEEK_END) == 0;
	const int64_t size = ok ? opennova::io::ftell64(file) : -1;
	ok = ok && size >= 0 && opennova::io::fseek64(file, 0, SEEK_SET) == 0;
	if (ok) {
		out.resize(size);
		ok = size == 0 ||
				std::fread(out.ptrw(), 1, static_cast<size_t>(size), file) ==
						static_cast<size_t>(size);
	}
	std::fclose(file);
	if (!ok) {
		out.clear();
		error = "cannot read " + path;
	}
	return ok;
}

void remove_file(const std::string &path) {
	std::error_code ignored;
	std::filesystem::remove(opennova::io::os_path(path), ignored);
}

// One worker's connection, kept open between its requests to one address.
struct Connection {
	Ref<HTTPClient> client;
	std::string origin;
	int64_t idle_since = 0;

	void close() {
		if (client.is_valid())
			client->close();
		origin.clear();
	}

	bool open_for(const Address &address) const {
		return client.is_valid() && origin == address.origin() && now_ms() - idle_since < kKeepMs &&
				client->get_status() == HTTPClient::STATUS_CONNECTED;
	}
};

class Worker {
public:
	Worker(Connection &connection, const ContentHttp::Exchange &exchange,
			const std::atomic<bool> &stopping) :
			connection_(connection), exchange_(exchange), stopping_(stopping) {}

	ContentResponse run();

private:
	bool given_up() const { return exchange_.cancelled || stopping_; }
	bool open(const Address &address, std::string &error);
	// Polled until the reply's head is in: false with `error` when none came, `retry` when the
	// connection failed before any of it did.
	bool await_head(int64_t sending_ms, bool &retry, std::string &error);
	bool read_body(ContentResponse &out, bool &lost);

	Connection &connection_;
	const ContentHttp::Exchange &exchange_;
	const std::atomic<bool> &stopping_;
};

bool Worker::open(const Address &address, std::string &error) {
	connection_.close();
	if (connection_.client.is_null()) {
		connection_.client.instantiate();
		connection_.client->set_blocking_mode(false);
		connection_.client->set_read_chunk_size(kChunkBytes);
	}
	Ref<TLSOptions> tls;
	if (address.tls)
		tls = TLSOptions::client();
	if (connection_.client->connect_to_host(opennova::to_gd(address.host), address.port, tls) !=
			OK) {
		error = "cannot reach " + address.host;
		return false;
	}
	const int64_t begun = now_ms();
	while (!given_up()) {
		connection_.client->poll();
		switch (connection_.client->get_status()) {
			case HTTPClient::STATUS_RESOLVING:
			case HTTPClient::STATUS_CONNECTING:
				break;
			case HTTPClient::STATUS_CONNECTED:
				connection_.origin = address.origin();
				return true;
			case HTTPClient::STATUS_CANT_RESOLVE:
				error = "could not find " + address.host;
				return false;
			case HTTPClient::STATUS_CANT_CONNECT:
				error = "could not connect to " + address.host;
				return false;
			case HTTPClient::STATUS_TLS_HANDSHAKE_ERROR:
				error = "the secure connection to " + address.host + " failed";
				return false;
			default:
				error = "the connection to " + address.host + " failed";
				return false;
		}
		if (now_ms() - begun > ContentHttp::kConnectMs) {
			connection_.close();
			error = address.host + " did not answer";
			return false;
		}
		std::this_thread::sleep_for(kWait);
	}
	connection_.close();
	error = "given up";
	return false;
}

bool Worker::await_head(int64_t sending_ms, bool &retry, std::string &error) {
	retry = false;
	const int64_t begun = now_ms();
	while (!given_up()) {
		const Error polled = connection_.client->poll();
		const HTTPClient::Status status = connection_.client->get_status();
		if ((status == HTTPClient::STATUS_BODY || status == HTTPClient::STATUS_CONNECTED) &&
				connection_.client->has_response())
			return true;
		if (status != HTTPClient::STATUS_REQUESTING || polled != OK) {
			connection_.close();
			retry = true;
			error = "the connection failed before the service answered";
			return false;
		}
		if (now_ms() - begun > sending_ms) {
			connection_.close();
			error = "the service did not answer";
			return false;
		}
		std::this_thread::sleep_for(kWait);
	}
	connection_.close();
	error = "given up";
	return false;
}

bool Worker::read_body(ContentResponse &out, bool &lost) {
	lost = false;
	const ContentRequest &request = exchange_.request;
	const bool to_file = !request.response_file.empty() && out.status >= 200 && out.status < 300;
	std::FILE *file = nullptr;
	if (to_file) {
		file = opennova::io::fopen_utf8(request.response_file.c_str(), "wb");
		if (file == nullptr) {
			connection_.close();
			out.error = "cannot write " + request.response_file;
			return false;
		}
	}
	opennova::io::Sha256 hash;
	bool ok = true;
	int64_t moved = now_ms();
	while (ok && connection_.client->get_status() == HTTPClient::STATUS_BODY) {
		if (given_up()) {
			out.error = "given up";
			ok = false;
			break;
		}
		connection_.client->poll();
		const PackedByteArray chunk = connection_.client->read_response_body_chunk();
		if (chunk.is_empty()) {
			if (now_ms() - moved > ContentHttp::kStalledMs) {
				out.error = "the download stalled";
				ok = false;
			}
			std::this_thread::sleep_for(kWait);
			continue;
		}
		moved = now_ms();
		const size_t size = static_cast<size_t>(chunk.size());
		if (file != nullptr) {
			if (std::fwrite(chunk.ptr(), 1, size, file) != size) {
				out.error = "cannot write " + request.response_file;
				ok = false;
			}
			hash.update(chunk.ptr(), size);
			out.file_bytes += size;
		} else {
			out.body.append(reinterpret_cast<const char *>(chunk.ptr()), size);
		}
	}
	const HTTPClient::Status status = connection_.client->get_status();
	if (ok && status != HTTPClient::STATUS_CONNECTED && status != HTTPClient::STATUS_DISCONNECTED) {
		out.error = "the connection failed during the reply (HTTP " + std::to_string(out.status) + ")";
		lost = true;
		ok = false;
	}
	if (file != nullptr) {
		ok = std::fclose(file) == 0 && ok;
		if (ok)
			out.file_sha256 = opennova::io::sha256_hex(hash.finish());
		else
			remove_file(request.response_file);
	}
	if (!ok) {
		connection_.close();
		return false;
	}
	if (status == HTTPClient::STATUS_DISCONNECTED)
		connection_.origin.clear();
	connection_.idle_since = now_ms();
	return true;
}

ContentResponse Worker::run() {
	const ContentRequest &request = exchange_.request;
	ContentResponse out;
	Address address;
	HTTPClient::Method method = HTTPClient::METHOD_GET;
	if (!split_url(request.url, address)) {
		out.error = "not an address the editor can reach: " + request.url;
		return out;
	}
	if (!method_of(request.method, method)) {
		out.error = "not a method the editor sends: " + request.method;
		return out;
	}
	PackedByteArray body;
	if (!request.body_file.empty()) {
		if (!read_whole(request.body_file, body, out.error))
			return out;
	} else if (!request.body.empty()) {
		body.resize(static_cast<int64_t>(request.body.size()));
		std::memcpy(body.ptrw(), request.body.data(), request.body.size());
	}
	PackedStringArray headers;
	bool has_length = false;
	for (const auto &[name, value] : request.headers) {
		headers.push_back(opennova::to_gd(name + ": " + value));
		has_length = has_length || opennova::strutil::iequals(name, "Content-Length");
	}
	// HTTPClient names a body's length only when there is one; a store refuses a PUT without it.
	if (body.is_empty() && !has_length && method != HTTPClient::METHOD_GET)
		headers.push_back("Content-Length: 0");
	const int64_t sending_ms = ContentHttp::kStalledMs + body.size() / kSlowestBytesPerMs;
	// A connection the far end breaks (a kept one it had closed, or one a store drops mid-reply, as
	// S3 does now and then over a push's thousands of uploads), or a store's 5xx, costs a request
	// that may go again another try on a fresh connection, kRetries times; any other request fails.
	for (int attempt = 0;; ++attempt) {
		out = ContentResponse();
		const bool kept = connection_.open_for(address);
		if (!kept && !open(address, out.error))
			return out;
		if (connection_.client->request_raw(
					method, opennova::to_gd(address.target), headers, body) != OK) {
			connection_.close();
			out.error = "the request to " + address.host + " could not be made";
			return out;
		}
		bool retry = false;
		if (!await_head(sending_ms, retry, out.error)) {
			if (retry && attempt < kRetries && idempotent(method))
				continue;
			return out;
		}
		out.status = connection_.client->get_response_code();
		const PackedStringArray lines = connection_.client->get_response_headers();
		for (int64_t i = 0; i < lines.size(); ++i) {
			const std::string line = opennova::to_std(lines[i]);
			const size_t colon = line.find(':');
			if (colon == std::string::npos)
				continue;
			size_t start = colon + 1;
			while (start < line.size() && (line[start] == ' ' || line[start] == '\t'))
				++start;
			out.headers.emplace_back(line.substr(0, colon), line.substr(start));
		}
		bool lost = false;
		out.transport_ok = read_body(out, lost);
		// A store that answers 5xx asks for the request again (S3's 500 and 503 SlowDown), after a
		// moment.
		const bool busy = out.transport_ok && out.status >= 500;
		if ((out.transport_ok && !busy) || (!lost && !busy) || attempt >= kRetries ||
				!idempotent(method))
			return out;
		if (busy)
			std::this_thread::sleep_for(std::chrono::milliseconds(500 * (attempt + 1)));
	}
}

} // namespace

ContentHttp::~ContentHttp() {
	{
		std::lock_guard<std::mutex> lock(mutex_);
		stopping_ = true;
		for (auto &[id, exchange] : running_)
			exchange->cancelled = true;
	}
	wake_.notify_all();
	for (std::thread &worker : workers_)
		worker.join();
}

uint64_t ContentHttp::start(opennova::editor::ContentRequest request) {
	auto exchange = std::make_shared<Exchange>();
	exchange->request = std::move(request);
	{
		std::lock_guard<std::mutex> lock(mutex_);
		exchange->id = next_++;
		queue_.push_back(exchange);
		while (workers_.size() < workers_wanted_)
			workers_.emplace_back([this] { run(); });
	}
	wake_.notify_one();
	return exchange->id;
}

bool ContentHttp::poll(uint64_t id, opennova::editor::ContentResponse &out) {
	std::lock_guard<std::mutex> lock(mutex_);
	const auto it = done_.find(id);
	if (it == done_.end())
		return false;
	out = std::move(it->second);
	done_.erase(it);
	return true;
}

void ContentHttp::cancel(uint64_t id) {
	std::lock_guard<std::mutex> lock(mutex_);
	for (auto it = queue_.begin(); it != queue_.end(); ++it) {
		if ((*it)->id == id) {
			queue_.erase(it);
			return;
		}
	}
	const auto running = running_.find(id);
	if (running != running_.end())
		running->second->cancelled = true;
	done_.erase(id);
}

void ContentHttp::run() {
	Connection connection;
	while (true) {
		std::shared_ptr<Exchange> exchange;
		{
			std::unique_lock<std::mutex> lock(mutex_);
			wake_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
			if (stopping_)
				break;
			exchange = queue_.front();
			queue_.pop_front();
			running_[exchange->id] = exchange;
		}
		ContentResponse response = Worker(connection, *exchange, stopping_).run();
		std::lock_guard<std::mutex> lock(mutex_);
		running_.erase(exchange->id);
		if (!exchange->cancelled)
			done_[exchange->id] = std::move(response);
	}
	connection.close();
}

} // namespace godot
