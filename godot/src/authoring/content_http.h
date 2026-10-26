#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include <editor/content_store/content_transport.h>

namespace godot {

// The content store's transport in the editor (ADR 0069;
// engine/editor/content_store/content_transport.h): each request is made over Godot's HTTPClient on
// one of a few worker threads, and each worker keeps its connection open between requests to the
// same address (a pull's downloads from one store, a push's uploads to it). A download's body is
// written to its file as it arrives, counted and hashed on the way, so no file is read twice; an
// upload's file is read whole into its request, since HTTPClient sends a request's body in one
// piece (request_raw). A reply is answered once it has all arrived, to the session's poll on the
// main thread; nothing here touches the scene tree. The workers start with the first request and
// end with the transport, a request still running given up.
class ContentHttp : public opennova::editor::ContentTransport {
public:
	explicit ContentHttp(size_t workers = 4) : workers_wanted_(workers == 0 ? 1 : workers) {}
	~ContentHttp() override;

	ContentHttp(const ContentHttp &) = delete;
	ContentHttp &operator=(const ContentHttp &) = delete;

	uint64_t start(opennova::editor::ContentRequest request) override;
	bool poll(uint64_t id, opennova::editor::ContentResponse &out) override;
	void cancel(uint64_t id) override;
	size_t parallel() const override { return workers_wanted_; }

	// A request in flight is given up once no byte of it moved for this long, and a connection once
	// it took this long to open.
	static constexpr int64_t kStalledMs = 120000;
	static constexpr int64_t kConnectMs = 30000;

	struct Exchange {
		uint64_t id = 0;
		opennova::editor::ContentRequest request;
		std::atomic<bool> cancelled{ false };
	};

private:
	void run();

	const size_t workers_wanted_;
	std::mutex mutex_;
	std::condition_variable wake_;
	std::deque<std::shared_ptr<Exchange>> queue_;
	std::map<uint64_t, std::shared_ptr<Exchange>> running_;
	std::map<uint64_t, opennova::editor::ContentResponse> done_;
	std::vector<std::thread> workers_;
	std::atomic<bool> stopping_{ false };
	uint64_t next_ = 1;
};

} // namespace godot
