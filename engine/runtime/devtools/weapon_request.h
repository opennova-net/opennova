// A typed mutation the F3 Weapon window queues for its embedder to drain (ADR
// 0042 d6; the DebugRequest pattern). The window never touches the FSM itself —
// the embedder routes each request into the engine's own weapon seams, and
// every trigger goes through the real request writer and its real gate
// (weapon_fsm_request_fire/_reload, weapon_fsm_queue_*), never a hand-rolled
// state write. Nothing here reaches the filesystem: edits are live-only.
#pragma once

#include <cstdint>

namespace opennova::devtools {

struct WeaponRequest {
	enum class Kind {
		// Patch one action's delaystart/delayend. Applied to the LIVE baked def
		// (instant, no re-bake, no slot disturbance — correct for dragging a
		// strip edge while the trigger is held) and mirrored into the retained
		// weapon.def row so it survives a re-install.
		SetActionDelays,
		// Patch one of the action's name fields. Sound/particle names apply
		// live; an anim change forces a re-bake, which re-resolves `auto`.
		SetActionText,
		// Queue an action on the local slot through its own request writer.
		TriggerAction,
		// The held-trigger latch (auto fire sustains through the FSM's own
		// deferred re-queue, so this is a hold, not a per-tick re-request).
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

	// SetActionDelays. -1 == `auto`; the embedder writes -1 into the retained
	// row and lets the re-bake resolve it from the clip.
	int32_t delay_start = 0;
	int32_t delay_end = 0;

	TextField field = TextField::Anim;  // SetActionText
	char text[128] = {};                // SetActionText payload

	Trigger trigger = Trigger::Fire;  // TriggerAction
	bool held = false;                // SetFireHeld
	bool armed = false;               // ArmTrace
};

}  // namespace opennova::devtools
