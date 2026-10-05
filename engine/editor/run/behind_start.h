#pragma once

#include <cstdint>
#include <map>
#include <vector>

namespace opennova::editor {

// The games Play starts behind (LaunchPlan::behind, the MCP gaps lane: play {behind}; the Shell's process platform,
// Windows only), kept behind while they start without holding the person's desktop: what each of the Shell's
// pumps does about each, from what its windows are then. Its first window is shown without activation (the
// platform's spawn). The foreground lock (no process takes the foreground: the only thing that stops a child of
// the foreground process from taking it) is taken at the spawn and held only while it starts: until each game
// started behind has shown a window and had it sent to the back once, or kLockMs passed, whichever is first
// (never while the person works on, review X12). Until kTendMs after its spawn, each pump sends a game's shown
// windows to the back, until one of them is the foreground window: the person brought it forward (or it took
// the foreground once the lock went), and it is left alone from then on, never pushed back under the person's
// other windows again. A game that exits is let go.
class BehindStarts {
public:
	static constexpr int64_t kLockMs = 3000;
	static constexpr int64_t kTendMs = 20000;
	enum class Step : uint8_t { Leave, SendBack };

	// The child `pid` started behind at `now_ms`.
	void spawned(int64_t pid, int64_t now_ms);
	// One pump's look at the child `pid`: whether it `exited`, whether a window of it is `shown`, and whether one
	// is the `foreground` window. False: it is let go (exited, brought forward, its time passed, or never started
	// behind); else `step` says what the pump does: Leave (no window shown yet) or SendBack (each shown window to
	// the back, without activation).
	bool tend(int64_t pid, int64_t now_ms, bool exited, bool shown, bool foreground, Step &step);
	// Whether the foreground lock still serves: a game started behind has shown no window sent back yet, within
	// kLockMs of its spawn. False: let it go now.
	bool lock_wanted(int64_t now_ms) const;
	// The games tended, in pid order.
	std::vector<int64_t> pids() const;
	bool tending() const { return !children_.empty(); }

private:
	struct Child {
		int64_t since = 0;
		bool sent_back = false;
	};
	std::map<int64_t, Child> children_;
};

} // namespace opennova::editor
