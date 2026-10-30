class_name NovaWorldSettings
extends RefCounted

# The NovaWorld menu's persisted choice of service: OpenNova's or the original NovaWorld (ADR
# 0010), stored under user:// across sessions. Every rule (the gates, the web site, the stored
# and launch choices) is the engine's (net/novaworld/service_target.h, NovaWorldServiceTarget).

const CONFIG_PATH := "user://novaworld_client.cfg"
const SECTION := "novaworld"

enum Target { OPENNOVA = 0, REAL = 1 }

# The gate's UDP port, shared by both targets (NovaWorldPanel's default).
const GATE_PORT := HostSessionConfig.DEFAULT_GATE_PORT


static func load_target() -> int:
	return NovaWorldServiceTarget.stored_target(
			ConfigStore.read(CONFIG_PATH, SECTION, "target", Target.OPENNOVA))


static func launch_target(flag_host: String, joining: bool) -> int:
	return NovaWorldServiceTarget.launch_target(flag_host, joining, load_target())


static func save_target(target: int) -> void:
	ConfigStore.write(CONFIG_PATH, SECTION, "target", target)
