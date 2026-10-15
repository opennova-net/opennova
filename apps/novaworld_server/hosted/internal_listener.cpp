#include "internal_listener.h"

#include "agent_route.h"

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <exception>
#include <mutex>
#include <utility>

namespace opennova::novaworld_server::hosted {

struct InternalListener::Impl {
	Impl(db::ConnectionPool &pool, FleetDesiredState &desired, AgentRouteConfig config) :
			routes(pool, desired, std::move(config)) {}

	crow::SimpleApp app;
	AgentRoutes routes;
	// start()'s handshake with the Crow thread, as HttpListener's: Pending
	// until Crow serves (Serving, with the port it bound) or until run()
	// returns (Exited).
	enum class Run { Pending, Serving, Exited };
	std::mutex run_mu;
	std::condition_variable run_cv;
	Run run = Run::Pending;
	uint16_t port = 0;
	static constexpr std::chrono::milliseconds kServingTick{ 50 };
};

InternalListener::InternalListener(
		db::ConnectionPool &pool, FleetDesiredState &desired, AgentRouteConfig config) :
		impl_(std::make_unique<Impl>(pool, desired, std::move(config))) {}

InternalListener::~InternalListener() {
	stop();
}

bool InternalListener::start(const std::string &address, uint16_t port) {
	if (running_.load())
		return true;
	Impl &impl = *impl_;
	impl.routes.register_routes(impl.app);
	// The embedder owns SIGINT and SIGTERM (main()'s orderly stop ends in
	// stop()); Crow would otherwise take both and stop this listener alone.
	impl.app.signal_clear();
	// Crow binds inside run(), on the worker thread; its tick runs on the
	// serving thread once the acceptor accepts, so the first tick hands start()
	// the port Crow took.
	impl.app.tick(Impl::kServingTick, [&impl] {
		std::lock_guard<std::mutex> lock(impl.run_mu);
		if (impl.run != Impl::Run::Pending)
			return;
		impl.run = Impl::Run::Serving;
		impl.port = impl.app.port();
		impl.run_cv.notify_all();
	});
	// Crow v1.2.0 runs `concurrency` - 1 handler threads beside its acceptor.
	impl.app.bindaddr(address).port(port).concurrency(
			static_cast<std::uint16_t>(kHandlerThreads + 1));
	worker_ = std::thread([&impl] {
		try {
			impl.app.run();
		} catch (const std::exception &e) {
			std::fprintf(stderr, "[fleet] the internal listener crashed: %s\n", e.what());
		}
		{
			std::lock_guard<std::mutex> lock(impl.run_mu);
			impl.run = Impl::Run::Exited;
		}
		impl.run_cv.notify_all();
	});
	bool serving = false;
	{
		std::unique_lock<std::mutex> lock(impl.run_mu);
		impl.run_cv.wait(lock, [&impl] { return impl.run != Impl::Run::Pending; });
		serving = impl.run == Impl::Run::Serving;
		bound_port_ = impl.port;
	}
	if (!serving) {
		worker_.join();
		return false;
	}
	running_.store(true);
	std::printf("[fleet] internal listener on %s:%u (the machine agents' routes)\n",
			address.c_str(), static_cast<unsigned>(bound_port_));
	return true;
}

void InternalListener::stop() {
	if (running_.exchange(false))
		impl_->app.stop();
	if (worker_.joinable())
		worker_.join();
}

} // namespace opennova::novaworld_server::hosted
