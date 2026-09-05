#pragma once

#include <runtime/inmatch/session.h>

namespace godot {

// The per-tick presentation sink (ADR 0035, ADR 0043 d9): the ONE typed hook
// the native session frame calls synchronously per catch-up tick, from inside
// Simulation::advance_session_frame / step_session_frame (the session's
// TickObserver::accept_tick), so Godot presentation consumes a tick before
// the next simulation tick runs. MissionRoot implements it; Simulation holds
// a bare pointer only for the duration of the frame call that installed it
// (Simulation::set_tick_sink). A false return declines the batch: the session
// ends the frame as SessionLost (runtime/inmatch/session.h TickObserver).
class TickSink {
public:
	virtual ~TickSink() = default;
	virtual bool on_session_tick(const opennova::inmatch::TickOutcome &p_tick) = 0;
};

} // namespace godot
