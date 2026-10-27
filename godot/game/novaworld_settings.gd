class_name NovaWorldSettings
extends RefCounted

# The game's NovaWorld: OpenNova's, the build's own deployment (ADR 0010 d2). The
# game picks no other (ADR 0075): no choice is offered or stored. See docs/adr/0010.

# The gate's UDP port (NovaWorldPanel's default); the engine-side session record
# owns the value.
const GATE_PORT := HostSessionConfig.DEFAULT_GATE_PORT
# OpenNova's gates (ADR 0010 d2): each deployment's own name (the
# unproxied A record at its gate's address, infra/aws/edge.tf; DEPLOY.md
# "Deployments: dev, staging and prod"): prod's, staging's, and loopback, the
# local stack DEVELOPING.md runs.
const OPENNOVA_NOVAWORLD_HOST := "nw.opennova.net"
const STAGING_NOVAWORLD_HOST := "nw.staging.opennova.net"
const LOCAL_NOVAWORLD_HOST := "127.0.0.1"

# The gate the game dials: --novaworld-host when given, else the
# export's own deployment, prod's in a release export and staging's in a debug
# one (PR CI's zip), as its updater's OPENNOVA.WIZ names (package_godot_windows.ps1),
# else loopback, so a source run (and every test) reaches the local stack and
# never a live service. Not stored: a build's own, never the player's.
static func opennova_host() -> String:
	return opennova_host_for(LaunchFlags.novaworld_host(), OS.has_feature("template"),
			OS.is_debug_build())

static func opennova_host_for(flag_host: String, exported: bool, debug: bool) -> String:
	if not flag_host.is_empty():
		return flag_host
	if not exported:
		return LOCAL_NOVAWORLD_HOST
	return STAGING_NOVAWORLD_HOST if debug else OPENNOVA_NOVAWORLD_HOST

# The NovaWorld site whose web API (/api/mods) belongs to the game's gate: a
# deployment's gate is nw.<domain> and its site https://<domain> (DEPLOY.md,
# the hosts the domain gives), and the local stack's gate is loopback with its
# site on the portal's port (DEVELOPING.md). The gate's own HTTP port answers no
# /api/ (DEPLOY.md: the portal's HTTPS server alone reaches it).
const LOCAL_WEB_BASE := "http://127.0.0.1:8000"

static func web_base() -> String:
	return web_base_for(opennova_host())

static func web_base_for(gate_host: String) -> String:
	if gate_host == LOCAL_NOVAWORLD_HOST:
		return LOCAL_WEB_BASE
	return "https://" + (gate_host.substr(3) if gate_host.begins_with("nw.") else gate_host)
