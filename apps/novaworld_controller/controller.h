#pragma once

// The controller: of a deployment's controller processes
// (apps/novaworld_controller/main.cpp), the one that runs the
// machines' internal listener, the hosted servers' supervisor, its admin
// links' console drain and the periodic jobs' scheduler (its parts), elected
// among them. Service policy, not a port.
//
// The election: a session advisory lock (db::SessionLock over
// db::kControllerLock) on a connection of the controller's own, outside the pool,
// tried again while another holds it (each try waits the connection's lock
// timeout); a standby waits there. Once it holds the lock the controller moves
// the generation in controller_leader on (migration 0008) and starts its
// parts in order. Then every check_ms it reads the row on that connection: the
// connection gone (and the lock with it) or a generation no longer its own is
// the leadership lost, and on_lost (the controller program's: the process exits,
// and its container starts it again as a standby). The parts' passes ask still_leads
// first (their guard), so a stale leader that has not noticed yet changes
// nothing once another leads: its passes do not start, and one under way asks
// again inside each write that enqueues cloud work (a machine's launch, its
// end), where the row's share lock orders that write before the next election
// or refuses it after. stop() stops the parts in reverse and lets the
// lock go, so a standby takes over within its lock timeout.
//
// The controller shares no memory with the other programs: what it keeps (the
// supervisor's runtimes, the started servers' files, the admin links) is its
// own, and what the others read of it is rows (the machines, the limits hold,
// the admin links' states, the console's answers).

#include <net/novaworld/db/database.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace opennova::novaworld_server::controller {

// The exit code of a process whose controller lost its leadership.
inline constexpr int kLeadershipLostExitCode = 71;

// What the controller runs while it leads.
struct ControllerPart {
	std::string name;
	std::function<void()> start;
	std::function<void()> stop;
};

struct ControllerConfig {
	// Who it is, in the row and the log.
	std::string holder;
	// How often a leader reads its row.
	int64_t check_ms = 1000;
	// Called once, on the controller's thread, when the leadership is lost.
	std::function<void(const std::string &why)> on_lost;
};

class Controller {
public:
	// `options` names the database; the controller opens its own connection.
	Controller(db::ConnectOptions options, ControllerConfig config);
	~Controller();
	Controller(const Controller &) = delete;
	Controller &operator=(const Controller &) = delete;

	// Before start(): the parts, started in order once it leads.
	void add_part(ControllerPart part);

	// The election and then the watch, on the controller's own thread.
	void start();
	// Its parts stopped (in reverse), the lock let go, the thread joined.
	void stop();

	bool leading() const { return leading_.load(); }
	int64_t generation() const { return generation_.load(); }
	// The election's and the watch's turns (the stall watchdog's counter while
	// it does not lead: a standby waiting is no stall).
	uint64_t turns() const { return turns_.load(std::memory_order_relaxed); }
	// The lock connection's server process (the tests end it).
	int backend_pid() const { return backend_pid_.load(); }

	// The passes' guard, on the caller's connection: it leads, and the row's
	// generation is still its own. Asked inside a transaction (a write that
	// enqueues cloud work), it holds the row's share lock to the transaction's
	// end: an election moving the generation on waits for that write, so the
	// new leader reads it, and a write after the election writes nothing.
	bool still_leads(db::Database &db) const;
	// Whether the row names generation `generation` (the row share-locked, as
	// above).
	static bool generation_holds(db::Database &db, int64_t generation);

private:
	void run();
	// Waits `ms`; true once stopping.
	bool wait_stop(int64_t ms);
	bool elect();
	void watch();

	const db::ConnectOptions options_;
	const ControllerConfig config_;
	std::vector<ControllerPart> parts_;
	size_t started_parts_ = 0; // the controller's thread's, then stop()'s

	// The controller's thread's alone.
	std::unique_ptr<db::Database> conn_;
	std::optional<db::SessionLock> lock_;

	std::atomic<bool> leading_{ false };
	std::atomic<int64_t> generation_{ 0 };
	std::atomic<uint64_t> turns_{ 0 };
	std::atomic<int> backend_pid_{ 0 };

	std::mutex mu_;
	std::condition_variable cv_;
	bool stopping_ = false;
	std::thread thread_;
};

} // namespace opennova::novaworld_server::controller
