# ADR 0010 — Completing the NovaWorld client (switchable OpenNova / real NovaWorld)

Status: accepted (roadmap + architecture; no leg implemented in this ADR)

## Context

Real NovaWorld is still live, and the retail game already switches between it and OpenNova through
the launcher's hosts redirect (see `launcher/`). We want **our Godot client** to do the same — point
at OpenNova *or* `gs.novaworld.net` — and, over time, implement the full client flow: browse, account
login, host, join.

Today the client (`godot/engine/network/nova_world_client.cpp`) does only the gate probe and the
session HELLO; the rest is stubbed:

- session AUTH hardcodes the server host-key (`send_session_join(/*server_hk=*/0)`, `nova_world_client.cpp:384`);
- the in-session layer-4 handler (opcode `0x83`) is an empty keep-alive note;
- the panel calls non-existent `get_server_rows()` / `host_game()` (`godot/game/novaworld_panel.gd`).

The important finding from surveying both repos: **almost all of the protocol and crypto already
exists.** Our `libs/` carry the verified crypto (NW-C1..C4 — NWU / EPASK / PUBcrypto / url_cipher, all
MATCHING vs retail), the envelope/TLV/container codecs (`libs/napi`), the `ServerHello` struct
including the `hk` field (`libs/novaworld/session_hello.h`), the `0x83` decoder
(`libs/novaworld/protocol_message.h`), and the client message builders
`make_client_host_request` / `make_client_host_update` / `make_client_play_request`
(`libs/napi/session.h`). The `opennova-int` reference (`onnw`) is **purely server-side** — no client
or packet-replay code — but it documents every leg to mirror. So "completing the client" is mostly
**client-side wiring plus a few client-direction IDA grills**, not new reverse engineering.

This ADR fixes the architecture and the order of work so the legs land cleanly. No leg is implemented
here.

## Decision

1. **The client lives in `libs/novaworld/client_session.{h,cpp}` — Godot-free.** It is the client
   mirror of `lobby_session`: a bytes-in / bytes-out state machine
   (`GATE → HELLO → AUTH/VERIFY → READY → {BROWSE, HOST, PLAY} → GOODBYE`) plus an event queue. The
   logic currently inline in the `NovaWorldClient` binding moves here; the binding becomes a thin
   pump (`PacketPeerUDP` + `HTTPRequest` + signals) that owns sockets only. This follows the engine
   convention (portable C++ in `libs/`, Godot wrapper in `engine/`) and makes every leg
   **unit-testable in ctest** via UDP/HTTP loopback against our own `apps/novaworld_server`,
   extending the `gate_server_loopback_test` pattern. No socket I/O lives in `libs/`.

2. **The endpoint is selectable, not hardcoded.** A persisted setting chooses the target: OpenNova
   (the configured/localhost host) or real NovaWorld (`gs.novaworld.net`, the existing
   `GATE_DEFAULT_HOST` in `libs/novaworld/gate_probe.h`). The client already exposes
   `host`/`gate_port`; the panel reads the setting instead of a hardcoded export. Primary target stays
   OpenNova; real NovaWorld is an opt-in parity/testing target — see Consequences.

3. **Reuse the libs, mirror `onnw`, grill only the client direction.** Each leg wires existing
   encoders/parsers/crypto, mirrors the corresponding `onnw` server module (inverting request/
   response), and adds a fresh IDA witness only for the *client-direction* behavior that has not been
   grilled yet. The crypto and all server-direction container parsing are already anchored
   (`docs/net/novaworld-net-re.md` §3/§8).

4. **Legs land in dependency order, one increment each:**
   - **Phase 0 — Endpoint switch.** Persisted OpenNova/real target + a picker in `novaworld_panel.gd`.
     No protocol, no grill. Unblocks live testing of every later leg.
   - **Phase 1 — Session auth/verify (foundation). [DONE]** Parse `ServerHello.hk` and echo it in
     `ClientAuth.hk` (removes the `=0` stub); wire `protocol_message.h` decode into the `0x83`
     handler. Target: the client reaches `ServerVerifyResult`. Landed as the Godot-free
     `libs/novaworld/client_session.{h,cpp}` (the binding is now a thin socket pump) plus the new
     `parse_server_hello` / `parse_server_auth` client-direction parsers; verified by
     `tests/novaworld/client_session_loopback_test` (HK echo + verify handshake to `Verified`).
     See `docs/net/novaworld-net-re.md` §7.1.
   - **Phase 2 — GSB browse (first visible win, read-only).** Client GSB query + parser (inverse of
     `gsb_build_response`; `onnw/gsb.py` chunks IVAR/FLDS/SUMMARY/SVRS/XXXX, key
     `3209452104342624532341`). Fills the panel list.
   - **Phase 3 — EPASK account login (the real-NW gate).** Client HTTP: read the EPASK `exp:mod:key`
     from the login page → POST `/NWLogin.dll` with `epask_encrypt(NAME/PASSWORD)` (NW-C2) + the form
     fields → parse `NWHANDLE`/`PCID` cookies (`onnw/controllers/nova_world/login.py` mirror). Needed
     for any authenticated action on real NovaWorld (real account).
   - **Phase 4 — Host-a-game.** HTTP host-register (`NWHost.dll`, `onnw/.../host.py`) + UDP
     `ClientHostRequest`/`ClientHostUpdate` (builders exist) → parse `ServerHostResult`.
   - **Phase 5 — Join.** HTTP join (`NWJoin.dll`, `onnw/.../join.py`: PUB\* via NW-C3, NK/CK via
     NW-C4, join ticket) + UDP `ClientPlayRequest` (builder exists) → parse `ServerPlayResult`; then
     the in-match seam of [ADR 0009](0009-in-match-net-seam.md).

5. **Verification per leg.** A ctest loopback (`client_session` ↔ `apps/novaworld_server`) is the
   gate for every leg — offline and deterministic. Live smoke against `gs.novaworld.net` (gate+hello,
   browser, login) and a two-client OpenNova host/join round-trip are the acceptance checks.

## Grill status (the "may need more IDA" axis)

- **Done:** NWU / EPASK / PUBcrypto / url_cipher (NW-C1..C4); gate / session-envelope / the ten
  NOVAWORLDUDP containers / `HandleClientHello` + `HandleClientJoin` (waves 1–2). All crypto and
  server-direction parsing are anchored to retail.
- **Still to witness (client direction):** the client GSB request format (P2), the retail client's
  `NWLogin` POST sequence and cookie handling (P3), the host/join HTTP→UDP handoffs (P4/P5), and the
  `ClientAuth.hk` echo (P1, likely already inferable from the anchored hello/join handlers). Nothing
  is blocked on un-grilled crypto.

## Consequences

- Every leg is unit-testable before it ever touches a live server, because the protocol lives in
  `libs/` and the binding is a thin pump. This is the main reason for the `client_session` extraction.
- Phase 0 alone makes the client a switchable OpenNova/real-NovaWorld client at the gate+hello level;
  each later phase widens what works against both targets with no rework, because they share the same
  state machine.
- The work reuses the already-verified crypto and the existing builders/parsers, so the surface area
  of *new* code per leg is small (mostly state wiring + the client HTTP flows).
- Posture: connecting a reimplemented client to NovaLogic's live service is done sparingly, for
  parity testing, without load or abuse. The retail executable remains the normal way to play on the
  official servers; OpenNova remains our primary target.
