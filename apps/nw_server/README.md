# nw-server — dev/golden-harness in-match host

A thin headless C++ binary that stands up the `engine/net/npruntime` in-match host
(the witnessed listen-server shape, a real UDP socket, the original 62 Hz
cadence) so the net iteration harness can produce golden-comparable sessions:
host a mission, let opennova or retail clients join, capture with dumpcap, and
diff against the retail golden. All protocol, crypto, and framing live in
`engine/net`; this binary owns only the socket and the cadence.

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
mission. The mission's authored mode normally selects the live game type. Set
`NW_GAME_TYPE` to an exact decimal or `0x`-prefixed `g_GameType` code when a
wire capture needs an explicit mode, and `NW_NUM_TEAMS=4` for retail's
four-side TDM, team KOTH, or FlagBall form. This override is also how the
harness reaches Flag Me (`0x8`): retail retains a type-12 load branch but its
BMS task-bit mapper has no path that returns 12
[`AI_GetTaskTypeFromFlags @0x40DAE0`; `Game_StartMission @0x524360`]. An
A&S/C&C capture probe can override the retail takeover defaults (15 seconds,
speed setting 1) with signed `NW_CAPTURE_DURATION_SECONDS` and
`NW_CAPTURE_SPEED_SETTING`; duration ≤0 selects retail's instant path
[`Config_SetDefaults @0x54D030`; `Server_UpdateCaptureZones @0x53B8F0`]. An
`NW_DEFAULT_SPAWN_REQUIRES_NO_TEAM_ZONE=1` probe enables retail cfg
`nodefaultspawnpoints`: target-less deployment is denied while the team has an
unnumbered or fully controlled numbered spawn zone
[`Server_ProcessClientRequestRespawn @0x519C8E`;
`Entity_HasAliveEntityOfTeam @0x4FC7B0`]. An
optional loose `score.ini` in the resource root overlays the retail default
score table and is rejected if malformed.
