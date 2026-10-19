#pragma once

// The console queue's drain (hosted/model/console_queue.h, whose routes' side is
// common): the fleet leader's, which holds the admin links (admin_link.h) the
// queued commands run on. Service policy, not a port.

#include "admin_link.h"

#include <net/novaworld/db/database.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <list>
#include <mutex>
#include <string>
#include <thread>

namespace opennova::novaworld_server::hosted {

// The fleet's side: claims the queued commands every poll_ms under its
// leader's generation (fleet_leader's when start() runs: the drain starts
// once the fleet program leads; 0 when driven by hand) and runs each on its
// link (none: LinkDown), writing its answer to its row; the rows whose routes
// went away (past their deadline a minute) are deleted, and those an earlier
// generation claimed and left running are answered LinkDown, not sent.
class ConsoleDrain {
public:
	// The most commands running at once; more wait queued.
	static constexpr size_t kMaxRunning = 64;

	// `pool` and `links` must outlive the drain.
	ConsoleDrain(db::ConnectionPool &pool, AdminLinks &links, int64_t poll_ms = 20);
	~ConsoleDrain();
	ConsoleDrain(const ConsoleDrain &) = delete;
	ConsoleDrain &operator=(const ConsoleDrain &) = delete;

	void start();
	// Ends the drain once the commands running have answered. Idempotent.
	void stop();
	// One claim of the queued commands, each started on a thread of its own
	// (the drain's turn; the tests call it by hand): how many it started.
	size_t drain_once();

private:
	struct Running {
		std::thread thread;
		std::atomic<bool> done{ false };
	};

	void run();
	void execute(int64_t id, int64_t server_id, std::string line, AdminCommandOptions options,
			std::chrono::milliseconds left);
	// Joins the commands that answered; how many still run.
	size_t reap();

	db::ConnectionPool &pool_;
	AdminLinks &links_;
	const int64_t poll_ms_;
	std::list<Running> running_; // the drain's thread alone
	int64_t last_cleanup_ = 0;
	int64_t generation_ = 0; // set by start(), before the drain's thread

	std::mutex mu_;
	std::condition_variable cv_;
	bool stopping_ = false;
	std::thread thread_;
};

} // namespace opennova::novaworld_server::hosted
