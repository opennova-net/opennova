# Track C: launcher (hosts-file model) and website

## Launcher

Port of the reference C# launcher (.NET 8 WinForms tray app) into top-level
`launcher/`, minus everything hook-related. The games' gate manager hard-codes the
hostname `gs.novaworld.net` (UDP 7597); the gate response controls every downstream
HTTP and UDP endpoint. Redirecting that one hostname is sufficient for JO, which the
hook era proved by patching exactly that string. The redirect hostname set is
server-driven (`GET /api/server-info`) so additions (DFX2, pending grill item NW-L1)
need no launcher release.

### Rules

- Requires administrator. `app.manifest` `requireAdministrator` plus a runtime assert;
  the launcher refuses to run unelevated. Launched games inherit elevation (accepted).
- Hosts management toggle in preferences. ON: a clearly delimited managed block is
  written to the hosts file and kept repaired. OFF: the block is removed and **game
  launch is disabled** (strict; no unredirected-launch escape hatch). Playing on a
  surviving real NovaWorld server means running the game outside the launcher.
- Dev mode points the managed block at 127.0.0.1; prod resolves the server IP via
  `/api/server-info` over the real internet, falling back to the cached IP, then a DNS
  lookup of `nw.<domain>` (a name we never redirect). Hosts entries are numeric IPv4.
- Launch is a plain `Process.Start` of the stock exe with stock flags: `/w` windowed,
  `/many` multi-instance, `/exp <slug>` expansions, plus per-game advanced args.
  No injector, no patched executables, no downloads of either.

### Hosts block

```
# >>> OpenNova Launcher managed block - do not edit between markers >>>
203.0.113.10 gs.novaworld.net
# <<< OpenNova Launcher managed block <<<
```

`HostsDocument` (pure parse/render, byte-preserves everything outside the markers,
preserves line endings) + `HostsFileStore` (atomic temp-and-replace, ReadOnly attribute
handling, retries) + `DnsCacheFlusher` (`ipconfig /flushdns`). Service states:
Disabled, Enabled, EnabledStaleIp (auto-repair), ForeignConflict (a foreign
`gs.novaworld.net` line outside our block: warn, confirm-gated cleanup), Inaccessible
(launch stays gated). Entries persist while the toggle is on, across launcher exits;
removed on toggle-off or the explicit "Remove hosts entries" button.

### Kept from the reference

Game detection, expansion manager (SHA-256 verified archives into `expansion/<slug>`),
preferences, auto-update (AutoUpdater.NET against `downloads.<domain>/launcher/
version.json`), backend games catalog. Deleted: `HookDeploymentService` and all hook
artifacts. Settings move to `%APPDATA%\OpenNovaLauncher`; version restarts at 0.2.0 as
a clean break (old 0.1.x installs reinstall manually; announced on the site).

### Tests

xunit project, headless on windows-latest: HostsDocument parse/replace/remove,
idempotency, line-ending and foreign-content preservation, corrupted markers; launch
argument mapping; settings round-trip and migration; catalog and server-info parsing
with fallback order. WinForms UI smoke stays manual (14-case matrix in the PR).

## Website

`web/` relands verbatim from the rescued PR #37 tree; it is a verified strict superset
of the reference onweb portal (wired register form, admin users page, bearer-token
admin gate). Served by the nginx container in compose (proxy `/api/`); the C++
server's static `web/dist` serving remains the documented no-docker fallback.

### Copy pass (PR 17)

- `/mod-tools` rewritten as a real tools section: leads with the portable C++ core and
  ONED, pre-1.0 framing, "JO and newer" naming, no em dashes, GitHub releases CTA.
- Landing page: remove the "applies game-improving patches" claims; the truthful story
  is one hosts entry, stock game files. Fix NovaLogic casing. Sweep em dashes.
- Register page stays live (POSTs the existing `/api/register`); copy clarified.
- Nav label becomes "Tools"; footer and GitHub links point at this repo.
