#include "controller.h"

#include <chrono>
#include <cstdio>
#include <exception>
#include <utility>

namespace opennova::novaworld_server::controller {

namespace {

using db::BindValue;

} // namespace

Controller::Controller(db::ConnectOptions options, ControllerConfig config) :
		options_(std::move(options)), config_(std::move(config)) {}

Controller::~Controller() {
	stop();
}

void Controller::add_part(ControllerPart part) {
	parts_.push_back(std::move(part));
}

void Controller::start() {
	if (thread_.joinable())
		return;
	{
		std::lock_guard<std::mutex> lock(mu_);
		stopping_ = false;
	}
	thread_ = std::thread([this] { run(); });
}

void Controller::stop() {
	{
		std::lock_guard<std::mutex> lock(mu_);
		stopping_ = true;
	}
	cv_.notify_all();
	if (thread_.joinable())
		thread_.join();
	for (size_t i = started_parts_; i > 0; --i) {
		const ControllerPart &part = parts_[i - 1];
		if (part.stop)
			part.stop();
	}
	if (started_parts_ > 0)
		std::printf("[controller] %s stopped its parts\n", config_.holder.c_str());
	started_parts_ = 0;
	leading_.store(false);
	lock_.reset(); // the lock let go: a standby takes over
	conn_.reset();
}

bool Controller::wait_stop(int64_t ms) {
	std::unique_lock<std::mutex> lock(mu_);
	return cv_.wait_for(lock, std::chrono::milliseconds(ms), [this] { return stopping_; });
}

bool Controller::generation_holds(db::Database &db, int64_t generation) {
	// FOR SHARE: inside a transaction the row stays locked to its end, so an
	// election's UPDATE (the generation moved on) waits for it and orders after
	// its writes; a read after that election finds no row.
	return !db.query("SELECT 1 FROM controller_leader WHERE id = 1 AND generation = $1 FOR "
					 "SHARE;",
					  { BindValue(generation) })
					.empty();
}

bool Controller::still_leads(db::Database &db) const {
	return leading_.load() && generation_holds(db, generation_.load());
}

bool Controller::elect() {
	std::string last_error;
	for (;;) {
		{
			std::lock_guard<std::mutex> lock(mu_);
			if (stopping_)
				return false;
		}
		turns_.fetch_add(1, std::memory_order_relaxed);
		try {
			if (!conn_) {
				db::ConnectOptions options = options_;
				options.application_name = "opennova-controller";
				conn_ = std::make_unique<db::Database>(options);
				const auto pid = conn_->query("SELECT pg_backend_pid();");
				backend_pid_.store(
						pid.empty() ? 0 : static_cast<int>(pid.front().as_int(0).value_or(0)));
			}
			// Waits the connection's lock timeout while another holds it.
			lock_.emplace(*conn_, db::kControllerLock);
			const auto rows = conn_->query(
					"UPDATE controller_leader SET generation = generation + 1, holder = $1, "
					"elected_at = now() WHERE id = 1 RETURNING generation;",
					{ BindValue(config_.holder) });
			generation_.store(rows.empty() ? 0 : rows.front().as_int(0).value_or(0));
			leading_.store(true);
			return true;
		} catch (const db::Error &e) {
			if (e.lock_timeout())
				continue; // another leads: try again
			if (last_error != e.what())
				std::fprintf(stderr, "[controller] WARN %s: the election: %s\n",
						config_.holder.c_str(), e.what());
			last_error = e.what();
			lock_.reset();
			conn_.reset();
			if (wait_stop(1000))
				return false;
		}
	}
}

void Controller::watch() {
	while (!wait_stop(config_.check_ms)) {
		turns_.fetch_add(1, std::memory_order_relaxed);
		std::string why;
		try {
			const auto rows =
					conn_->query("SELECT generation, holder FROM controller_leader WHERE id = 1;");
			const int64_t generation = rows.empty() ? 0 : rows.front().as_int(0).value_or(0);
			if (generation != generation_.load())
				why = "another controller leads (generation " + std::to_string(generation) + ", " +
						rows.front().as_text(1).value_or("") + ")";
		} catch (const std::exception &e) {
			why = std::string("its lock's connection failed: ") + e.what();
		}
		if (why.empty())
			continue;
		leading_.store(false);
		std::fprintf(stderr, "[controller] FATAL %s lost the leadership (generation %lld): %s\n",
				config_.holder.c_str(), static_cast<long long>(generation_.load()), why.c_str());
		if (config_.on_lost)
			config_.on_lost(why);
		return;
	}
}

void Controller::run() {
	std::printf("[controller] %s stands for election\n", config_.holder.c_str());
	if (!elect())
		return;
	std::printf("[controller] %s leads (generation %lld)\n", config_.holder.c_str(),
			static_cast<long long>(generation_.load()));
	for (const ControllerPart &part : parts_) {
		{
			std::lock_guard<std::mutex> lock(mu_);
			if (stopping_)
				return;
		}
		if (part.start)
			part.start();
		++started_parts_;
	}
	watch();
}

} // namespace opennova::novaworld_server::controller
