// A typed mutation the F3 Weapon window queues for its embedder to drain (ADR
// 0042 d6). The window never touches the FSM itself — the embedder routes each
// request into the engine's own weapon seams, and every trigger goes through
// the real input seam and its real gate, never a hand-rolled state write.
// Nothing here reaches the filesystem: edits are live-only. A window-local
// seam: the dope-sheet edits have no debug-control row, so they stay beside
// the ControlRequest channel rather than in it.
#pragma once

#include <cstdint>

namespace opennova::devtools {

struct WeaponRequest {
	enum class Kind {
		// Patch one action's delays, in AUTHORED form: -1 is `auto`. The
		// embedder mirrors both legs into the retained weapon.def row as given
		// (so an untouched `auto` leg stays `auto`), patches the LIVE baked slot
		// only for the explicit legs (instant, no re-bake, no slot disturbance —
		// correct for dragging a strip edge while the trigger is held), and
		// re-bakes only when `rebake` says a leg newly became `auto` and has to
		// resolve from the clip.
		SetActionDelays,
		// Patch one of the action's name fields. Sound/particle names apply
		// live; an anim change re-bakes, which re-resolves `auto`.
		SetActionText,
		// Queue an action through its real input seam.
		TriggerAction,
		// The held-trigger latch (auto fire sustains through the FSM's own
		// deferred re-queue, so this is a hold, not a per-tick re-request). The
		// window clears it whenever it hides: a hold cannot outlive the UI that
		// shows it.
		SetFireHeld,
		// Arm/disarm and clear the 62.5 Hz trace ring.
		ArmTrace,
		ClearTrace,
	};

	// Which name field SetActionText writes.
	enum class TextField {
		Anim,
		SoundSet,
		SoundSetEnd,
		Particle,
		ParticleUserPoint,
	};

	// What TriggerAction asks for. These are the REAL input-dispatcher seams —
	// the same ones the mouse and the keyboard reach — not the FSM's internal
	// queue writers, so every witnessed gate and queueing rule still applies.
	// Scope is a toggle and weapon selection is a cycle because that is what
	// the bindings actually are.
	enum class Trigger {
		Fire,
		Reload,
		ScopeToggle,
		NextWeapon,
		PrevWeapon,
	};

	Kind kind = Kind::SetActionDelays;
	int32_t action_id = 0;  // weapon_action::* (SetActionDelays, SetActionText)

	// SetActionDelays, authored form (-1 == `auto`).
	int32_t delay_start = 0;
	int32_t delay_end = 0;
	bool rebake = false;

	TextField field = TextField::Anim;  // SetActionText
	char text[128] = {};                // SetActionText payload

	Trigger trigger = Trigger::Fire;  // TriggerAction
	bool held = false;                // SetFireHeld
	bool armed = false;               // ArmTrace
};

}  // namespace opennova::devtools
