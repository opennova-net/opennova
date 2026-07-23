class_name NovaWorldSettings
extends RefCounted

# Persisted client-side settings for the NovaWorld front-end. For now just the
# target endpoint: OpenNova (our server) vs the original NovaWorld (NovaLogic's
# live gate). Stored under user:// so it survives across sessions. Later client
# legs (login, last server, etc.) can grow here. See docs/adr/0010.

const CONFIG_PATH := "user://novaworld_client.cfg"
const SECTION := "novaworld"

enum Target { OPENNOVA = 0, REAL = 1 }

# The original NovaWorld gate host — matches GATE_DEFAULT_HOST in
# libs/novaworld/gate_probe.h. The OpenNova host is configured per build/run
# (dev: localhost), so it is not stored here. GATE_PORT is the gate's UDP port,
# shared by both targets (NovaWorldPanel's default).
const REAL_NOVAWORLD_HOST := "gs.novaworld.net"
const GATE_PORT := 7597

static func load_target() -> int:
	var cfg := ConfigFile.new()
	cfg.load(CONFIG_PATH)  # a missing file is fine; get_value returns the default
	var value := int(cfg.get_value(SECTION, "target", Target.OPENNOVA))
	if value != Target.OPENNOVA and value != Target.REAL:
		return Target.OPENNOVA
	return value

static func save_target(target: int) -> void:
	var cfg := ConfigFile.new()
	cfg.load(CONFIG_PATH)
	cfg.set_value(SECTION, "target", target)
	cfg.save(CONFIG_PATH)
