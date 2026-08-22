# nw-server — dev/golden-harness in-match host

A thin headless C++ binary that stands up the `engine/net/npruntime` in-match host
(the witnessed listen-server shape, a real UDP socket, the original 62 Hz
cadence) so the net iteration harness can produce golden-comparable sessions:
host a mission, let opennova or retail clients join, capture with dumpcap, and
diff against the retail golden. All protocol, crypto, and framing live in the
libs; this binary owns only the socket and the cadence.

It is a development and golden-harness tool. It is **never packaged or
shipped**: dedicated hosting for players is a serve mode of `opennova.exe`,
not a third product
([ADR 0015](../../docs/adr/0015-two-products-serve-mode.md)).

- The runtime it drives: [`engine/net/npruntime/ROADMAP.md`](../../engine/net/npruntime/ROADMAP.md)
  (build plan + live status).
- The capture/diff loop it exists for: [`scripts/net/README.md`](../../scripts/net/README.md).

Launch with `NW_MISSION` pointing at a loose `.bms`. The mission's directory is
the default resource root for its `.env` and the retail WAC layers
`game.wac`, `server.wac`, and `<mission>.wac`. Use `NW_ENV` to override only the
environment file, or `NW_RESOURCE_ROOT` when the shared WAC layers were exported
to a different directory. A present WAC layer that cannot be read or compiled
aborts before the UDP socket opens; having no WAC layers is a valid BMS-only
mission. The mission's authored mode selects the live game type (including TDM,
Advance and Secure, and co-op); an optional loose `score.ini` in the resource
root overlays the retail default score table and is rejected if malformed.
