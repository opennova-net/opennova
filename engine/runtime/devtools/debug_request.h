// A typed mutation an F3 window queues for its embedder to drain (ADR 0042
// d6; the GameWindowRequest pattern with a payload). The window never mutates
// engine state itself — the embedder routes each request into the same
// engine-backed debug delegates the MCP control plane uses.
#pragma once

#include <cstdint>

#include <runtime/world/entity.h>

namespace opennova::devtools {

struct DebugRequest {
	enum class Kind {
		// The scripted-SETHP stores on the AI entity behind `target`.
		SetEntityHealth,
		// Both position stores (registry + AI motor mirror) behind `target`,
		// mission-space `pos`.
		SetEntityPosition,
		// The engine's full local-player teleport transaction: mission-space
		// `pos` plus mission `yaw`/`pitch` degrees; `target` unused.
		TeleportLocalPlayer,
	};

	Kind kind = Kind::SetEntityHealth;
	world::EntityHandle target{};       // packed wire handle (SetEntity*)
	int32_t health = 0;                 // SetEntityHealth
	float pos[3] = {0.0f, 0.0f, 0.0f};  // mission space (Z-up)
	float yaw = 0.0f;                   // TeleportLocalPlayer, mission degrees
	float pitch = 0.0f;                 // TeleportLocalPlayer, mission degrees
};

}  // namespace opennova::devtools
