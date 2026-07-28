# Retail Interop and Packet Capture

Use this file when starting local OpenNova, retail JO, packet capture, replay,
or NovaWorld redirection work.

## Topology Choices

Pick and record one topology before launching anything:

- Same-machine retail and native/local server:
  `ONNET_PUBLIC_HOST=127.0.0.1`, launcher Developer mode on, managed hosts entry
  maps `gs.novaworld.net` to `127.0.0.1`.
- Second-machine retail:
  `ONNET_PUBLIC_HOST=<server LAN IP or EIP>`, and the retail machine must resolve
  `gs.novaworld.net` to that reachable address. Do not use `127.0.0.1` on a
  second retail machine.
- Docker/dev stack:
  override the dev compose example IP. Set both `ONNET_PUBLIC_HOST` and
  `ONNET_CLIENT_REFLECT_IP` to the actual reachable adapter IP for the run.

Ports to check:

- `ONNET_GATE_UDP_PORT`, default `7597/udp`
- `ONNET_NW_UDP_PORT`, default `64206/udp`
- `ONNET_HTTP_PORT`, default `8080/tcp`
- Vite dev UI, default `5173/tcp`
- Retail host reflection defaults:
  `ONNET_CLIENT_REFLECT_GATE_PORT=49152` and
  `ONNET_CLIENT_REFLECT_NOVAWORLD_PORT=32768`

Launcher variables:

- `ONLAUNCHER_API_BASE_URL`
- `ONLAUNCHER_NW_ANCHOR_HOST`
- `ONLAUNCHER_UPDATE_MANIFEST_URL`

Godot direct-launch variables:

- Host: `NW_LAN_HOST`, `NW_LAN_PORT`, `NW_GATE_HOST`, `NW_GATE_PORT`,
  `NW_LAN_NAME`
- Joiner: `NW_LAN_JOIN`, `NW_LAN_MISSION`, `NW_LAN_NAME`
- Replay spectator: `NW_REPLAY`, `NW_REPLAY_DIR`, `NW_REPLAY_LOOSE`,
  `NW_REPLAY_ITEMS`

## What Can Be Automated Today

- OpenNova server startup and API smoke checks.
- Godot/OpenNova listen host and joiner through `NW_LAN_*` env hooks.
- NovaWorld host registration for a Godot listen host.
- Retail launch with stock files through the launcher after hosts redirection.
- API polling through `/api/server-info`, `/api/hosts`, `/api/lobbies`, and
  `/api/unknowns`.
- Packet decode with `nw_pp` and replay/spectator streaming with `nw_replay`.

Retail menu navigation, credentials, choosing host/join rows, and deterministic
in-match control still require a human operator or a future external UI driver.
Do not jump directly to an injected DLL. Keep any future retail driver opt-in,
research-only, version-gated, and separate from the public launcher.

## Capture Policy

- Prefer `.pcapng` in `.scratch/`.
- Capture from process start through the behavior under study. Mid-stream
  captures usually miss the handshake and cannot recover SCRK/session keys.
- Keep raw retail captures, `.sph` logs, account names, local paths, and machine
  IPs out of tracked files.
- Promote only small sanitized fixtures such as `.nwmsg`, focused `.hexcap`,
  `.gsb`, or manifest rows.

Supported inputs:

- `nw_pp <capture.pcapng>` or `nw_pp <capture.pcap>`
- `nw_pp <capture.hexcap>`
- `nw_pp <host.sph>` or `nw_pp <client.sph>`
- `nw_replay <capture.pcapng> --print-roles`
- `nw_replay <capture.pcapng> --validate --items <items.def>`

Useful env-gated witnesses:

- `NW_INGAME_HEXCAP`
- `NW_PROFILE_SPH_DIR`
- `NW_DVXI5_PCAP`, `NW_DVXI3_PCAP`, `NW_DVXC1_PCAP`
- `NW_PROBE3AGAIN_PCAP`, `NW_PROBE3AGAIN_HOST_SPH`
- `NW_WHITENOISE_PCAP`

## Packet-Diff Matrix

Capture comparable runs for:

- Retail host + retail joiner
- OpenNova host + retail joiner
- Retail host + OpenNova joiner
- OpenNova host + OpenNova joiner

Decode with `nw_pp --stream --items <items.def>` and compare decoded direction,
frame, session, tag, length, and fields. Do not compare encrypted raw bytes as
the primary signal.

For host/join load failures, focus on:

`0x0B`, `0x10`, `0x0D`, `0x0C`, `0x20`, `0x42`, `0x0A`, `0x0F`,
`0x61`, `0x25`, and `0x1A`.

Expected retail-compatible host signatures:

- Early self `0x0C` organic spawn during world load.
- Required dynamic world stream only.
- No pre-deploy S2C `0x25`.
- Game-start bundle only after the load gate.

Failure signatures to report:

- Retail joiner never emits C2S `0x0C` deploy uplinks.
- Retail joiner floods short C2S `0x0F` item resync requests.
- GOODBYE/kick follows deploy gate.
- Terrain/map missing while dynamic actors still appear.

## Initial-Load Failure Triage

For a retail client that joins an OpenNova host but floats or sees no map
entities, do not start with a broad refactor. First capture the full handshake
and inspect exact S2C order around:

`0x10`, live `0x0C`, live `0x20`, `0x45`, `0x7E`, `0x1A`, and `0x0F`.

Then verify in order:

- Mission identity and local `.bms` availability.
- Terrain/load `0x45` pages for the chosen mission.
- Spawn position in `0x0F` against the mission start marker and terrain height.
- Spawn-marker `0x20` records, preserving original pool indices.
- Joiner self-ID/DCB: the self `0x0C` is sent before deploy with the retail
  ack value and local-player flags.

Do not reintroduce full static pool streaming as a blanket fix. Retail clients
load static mission data locally; the host stream should be the required dynamic
state and spawn markers.

## Paths Not To Confuse

- `NovaNetClient` / `NetWorldView` is replay/spectator receive plumbing, not the
  canonical co-op gameplay client.
- `ClientSession` is the NOVAWORLDUDP lobby verifier/proto-switch client, not
  in-match replication.
- `UdpSessionTransport` is an internal identity-frame conduit. NWU session
  framing, SCRK, and ProtocolMessage live in `libs/npwire` (ADR 0019); the PN
  classifier (`classify_session_protocol`) stays in `libs/novaworld`.


## Known RETAIL-side defects — do NOT chase these as ours

Symptoms that look like our wire output but are the original engine misbehaving.
Each was live-A/B'd retail↔retail before being written down. If you see one,
stop: it is not a bug in OpenNova.

- **The host's own first-person weapon reacts when another player fires the SAME
  weapon** (D-NET-184, net-re §5.67). Listen-host only, first-person only, stops
  the instant either side switches weapon, never affects the non-authority side,
  and happens firing into the air. The host poses its viewmodel straight out of
  the shared per-weapon-type `WeaponDef+372` anim object
  [orig: `Player_RenderFirstPersonViewModel @0x4DEF75`], and `WeaponAction_Idle`
  re-seeds that object with **no owner guard** [orig: `@0x542955`] while all three
  sibling plays ARE guarded on `ownerEntity == g_local_player_entity`
  [orig: `@0x541893`/`@0x54195A`/`@0x5419B8`]. Two entities holding one weapon
  share one `Def` pointer **inside the host's process** — the joiner's process is
  not involved at all; a script emitting valid C2S `0x06` reproduces it.
  **Confirmed retail-native 2026-07-26** with two stock clients
  (`.scratch/golden/retail-retail-same-weapon-viewmodel-ab.pcapng`).
  Two of our own wire bugs were found and fixed while chasing this and NEITHER
  was the cause — do not re-open them as suspects: the `hit_part` packing
  (D-WPN-8) and C2S `0x06` off32 (D-WPN-8). Both are real divergences, both are
  fixed, and the symptom survived each.

**Method note that cost most of a session.** The first retail↔retail capture was
used to argue "this does not happen retail↔retail". It could not support that:
only ONE entity ever fired in it and the two players held DIFFERENT primaries, so
the same-weapon precondition was never exercised. **Before treating a control run
as a falsification, verify it actually exercised the precondition.**
