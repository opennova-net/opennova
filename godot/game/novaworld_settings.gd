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
# engine/net/novaworld/gate_probe.h. GATE_PORT is the gate's UDP port, shared by
# both targets (NovaWorldPanel's default); the engine-side session record owns
# the value.
const REAL_NOVAWORLD_HOST := "gs.novaworld.net"
const GATE_PORT := HostSessionConfig.DEFAULT_GATE_PORT
# The OpenNova target's gates (ADR 0010 d2): the live service's own name (the
# unproxied A record at the gate's address, infra/aws/edge.tf), and loopback, the
# local stack DEVELOPING.md runs.
const OPENNOVA_NOVAWORLD_HOST := "nw.opennova.net"
const LOCAL_NOVAWORLD_HOST := "127.0.0.1"

# The gate the OpenNova target dials: --novaworld-host when given, else the live
# service in an exported build, else loopback, so a source run (and every test)
# reaches the local stack and never the live service. Not stored: a build's
# own, never the player's.
static func opennova_host() -> String:
	return opennova_host_for(LaunchFlags.novaworld_host(), OS.has_feature("template"))

static func opennova_host_for(flag_host: String, exported: bool) -> String:
	if not flag_host.is_empty():
		return flag_host
	return OPENNOVA_NOVAWORLD_HOST if exported else LOCAL_NOVAWORLD_HOST

static func load_target() -> int:
	var value := int(ConfigStore.read(CONFIG_PATH, SECTION, "target", Target.OPENNOVA))
	if value != Target.OPENNOVA and value != Target.REAL:
		return Target.OPENNOVA
	return value

static func save_target(target: int) -> void:
	ConfigStore.write(CONFIG_PATH, SECTION, "target", target)
