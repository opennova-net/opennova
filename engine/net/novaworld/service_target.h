#pragma once

// THE NOVAWORLD SERVICE THE MENU DIALS: OpenNova's own (ADR 0010 d2) or NovaLogic's original
// NovaWorld, and the addresses each one's gate and web site sit at. OpenNova policy over one
// retail fact, the original gate's host. The player's stored choice is the embedder's to persist.

#include <cstdint>
#include <optional>
#include <string>

namespace opennova {

// The menu's choice, as it is stored (the values are the stored form).
enum class NovaWorldTarget : int {
	OpenNova = 0,
	Original = 1,
};

// The OpenNova target's gates: each deployment's own name (the unproxied A record at its gate's
// address, infra/aws/edge.tf; DEPLOY.md "Deployments: dev, staging and prod"), prod's and
// staging's, and loopback, the local stack DEVELOPING.md runs.
inline constexpr const char *kOpenNovaGateHost = "nw.opennova.net";
inline constexpr const char *kStagingGateHost = "nw.staging.opennova.net";
inline constexpr const char *kLocalGateHost = "127.0.0.1";
// A local stack's web portal port: its gate's own HTTP port answers no /api/ (DEPLOY.md, the
// portal's HTTPS server alone reaches it).
inline constexpr int kLocalWebPort = 8000;

// The gate the OpenNova target dials: `flag_host` (--novaworld-host) when given, else the
// build's own deployment, prod's in a release export and staging's in a debug one (PR CI's zip),
// as its updater's OPENNOVA.WIZ names (package_godot_windows.ps1), else loopback, so a source run
// (and every test) reaches the local stack and never a live service. Never stored: a build's
// own, never the player's.
std::string opennova_gate_host(const std::string &flag_host, bool exported, bool debug);

// The gate a target dials: the original's (net/novaworld/gate_probe.h GATE_DEFAULT_HOST), else
// `opennova_host` (opennova_gate_host's).
std::string target_gate_host(NovaWorldTarget target, const std::string &opennova_host);

// NovaLogic's NovaWorld: novaworld.net and its hosts (gs.novaworld.net), any case.
bool is_novalogic_host(const std::string &host);

// The NovaWorld site whose web API (/api/mods) belongs to the OpenNova target's gate, whichever
// target the menu is on (the original NovaWorld has no such API): a deployment's gate is
// nw.<domain> and its site https://<domain>; a local stack's gate is an address (loopback, or a
// LAN address on a second machine) with its site on kLocalWebPort, an IPv6 address bracketed.
// "" for NovaLogic's gate, which serves no mods: nothing is asked of it.
std::string service_web_base(const std::string &gate_host);

// The stored choice read back: OpenNova's or the original's; anything else (nothing stored, a
// value out of range) is OpenNova's.
NovaWorldTarget stored_novaworld_target(std::optional<int64_t> stored);

// The target a launch's NovaWorld menu opens on: the stored choice, except that a launch naming a
// gate (--novaworld-host, a developer's launch at a stack) opens on OpenNova's, so a stored pick
// never sends it to NovaLogic's live gate. A join's relaunch after a mod's install
// (--novaworld-join, `joining`) returns to the stored choice, which the menu stores as the join's
// target before the install.
NovaWorldTarget launch_novaworld_target(
		const std::string &flag_host, bool joining, NovaWorldTarget stored);

} // namespace opennova
