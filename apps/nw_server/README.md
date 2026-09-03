# nw-server — dev/golden-harness in-match host

A thin headless C++ binary that boots a loose mission through the engine's one
`mission::MissionKernel` (ADR 0042 d3) and stands up the `engine/runtime/inmatch`
in-match host through `inmatch::HostRole` (the witnessed dedicated
host-only shape, a real UDP socket, the original 62 Hz cadence) so the net
iteration harness can produce golden-comparable sessions: host a mission, let
opennova or retail clients join, capture with dumpcap, and diff against the
retail golden. The kernel boot loads what the mission names from the mounted
resource root — terrain (`.cpt`/`.trn` + charmap, with the raw `.til` bytes
feeding the S2C 0x45 terrain-tile stream for wire joiners), the
items/weapon/ammo tables, collision instances, infantry `.adm` grounding, and
the layered WAC scripts. All protocol, crypto, framing, mission state, and
cadence live in the libs; this binary owns only the flag surface, the socket,
and the wall-clock pacing.

It is a development and golden-harness tool. It is **never packaged or
shipped**: dedicated hosting for players is a serve mode of `opennova.exe`,
not a third product
([ADR 0015](../../docs/adr/0015-two-products-serve-mode.md)). Stock retail
clients joining it remains the retail-join follow-up: the per-join legs a
retail client exercises beyond the opennova client's are not yet golden-proven
against this host.

- The runtime it drives: [`engine/runtime/inmatch/ROADMAP.md`](../../engine/runtime/inmatch/ROADMAP.md)
  (build plan + live status).
- The capture/diff loop it exists for: [`scripts/net/README.md`](../../scripts/net/README.md).

Every setting is a command-line flag (`nw_server --help` lists them); nothing
is read from the environment. Launch with `--mission <path.bms>` naming a loose
`.bms`. The mission's directory is the default resource root; the root is
mounted the way the kernel mounts a game install (a `.pff` set with loose
overrides when archives exist, the loose tree otherwise — loose files win), and
everything except the mission and its `.env` resolves through that mount. Use
`--env <path.env>` to override only the environment file (the default is
`<environment>.env` beside the mission), or `--resource-root <dir>` when the
mission's resources were exported to a different directory. The WAC walk is
the engine's layered load (`game.wac` -> `server.wac` -> `<mission>.wac`) in
STRICT mode: any WAC diagnostic — not just a failed compile — aborts before
the UDP socket opens, because running a partial script is a known wire-parity
failure; having no WAC layers is a valid BMS-only mission. Missing terrain or
tables log a warning and the boot continues, exactly like the other kernel
embedders. `--port <n>` binds something other than the retail LAN range head.
The mission's authored mode normally selects the live game type. Pass
`--game-type <code>` with an exact decimal or `0x`-prefixed `g_GameType` code
when a wire capture needs an explicit mode, and `--num-teams 4` for retail's
four-side TDM, team KOTH, or FlagBall form. This override is also how the
harness reaches Flag Me (`0x8`): retail retains a type-12 load branch but its
BMS task-bit mapper has no path that returns 12
[`AI_GetTaskTypeFromFlags @0x40DAE0`; `Game_StartMission @0x524360`]. An
A&S/C&C capture probe can override the retail takeover defaults (15 seconds,
speed setting 1) with signed `--capture-duration-seconds` and
`--capture-speed-setting`; duration ≤0 selects retail's instant path
[`Config_SetDefaults @0x54D030`; `Server_UpdateCaptureZones @0x53B8F0`].
`--spawn-wave-time-base` and `--spawn-wave-time-zone` override the spawn-wave
timing the same way. A `--default-spawn-requires-no-team-zone` probe enables
retail cfg `nodefaultspawnpoints`: target-less deployment is denied while the
team has an unnumbered or fully controlled numbered spawn zone
[`Server_ProcessClientRequestRespawn @0x519C8E`;
`Entity_HasAliveEntityOfTeam @0x4FC7B0`]. `--log-debug` forwards the
libraries' `io/log.h` kDebug tracing (the per-tick burst trace) to the console
sink. An optional mounted `score.ini` overlays the retail default score table
and is rejected if malformed; a mounted `gametext.bin` supplies the "Server"
chat strings (`STRSRV_MEDREQ`).
