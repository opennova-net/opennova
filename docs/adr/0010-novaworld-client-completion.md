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
including the `hk` field (`libs/npwire/session_hello.h`), the `0x83` decoder
(`libs/npwire/protocol_message.h`), and the client message builders
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
   `host`/`gate_port`; the panel reads the setting instead of a hardcoded export. Wire compatibility
   is a standing design requirement either way (the client speaks the protocol such that it can join
   original servers, exactly as our server serves original clients and opennova↔opennova works). What
   is opt-in and done sparingly is *pointing the client at NovaLogic's **live hosted** service*
   (`gs.novaworld.net`) — a parity/testing target, not the everyday path; opennova↔opennova remains the
   primary product path. See Consequences.

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
   - **Phase 2 — GSB browse (first visible win, read-only). [DONE]** Client GSB query + parser
     (inverse of `gsb_build_response`; chunks IVAR/FLDS/SVRS/SVRS/XXXX, key `3209452104342624532341`).
     Landed as `gsb_parse_response` in `libs/novaworld/gsb.{h,cpp}` (verified by `gsb_parse_roundtrip`)
     plus the binding's HTTP browse leg: a Godot `HTTPRequest` child fetches `<startup_url>/jop_2.gsb`
     on reaching Verified, parses it, and exposes `get_server_rows()` + a `server_list_updated` signal
     that fills the panel list. Grill NW-G3 (`docs/net/novaworld-net-re.md` §8): the client GSB
     *request* URL is not a binary literal — the client GETs a server-provided URL, so OpenNova
     matches by construction (the cookie question for real NW is carried to Phase 3).
   - **Phase 3 — EPASK account login (the real-NW gate). [protocol core landed; host wiring pending]**
     Client HTTP: read the EPASK `exp:mod:key` from the login page → POST `/NWLogin.dll` with
     `epask_encrypt(NAME/PASSWORD)` (NW-C2) + the form fields → parse `NWHANDLE`/`PCID` cookies
     (`onnw/controllers/nova_world/login.py` mirror). Needed for any authenticated action on real
     NovaWorld (real account). The Godot-free pieces — `build_credentials_post_body`,
     `parse_set_cookie_values`, `CookieJar` — landed as `libs/novaworld/http_login.{h,cpp}`, verified by
     `tests/novaworld/http_login_test` against the server's own decode path. The wire contract is
     witnessed (grill NW-S5/B): POST `application/x-www-form-urlencoded`, cookies ride every subnet-keyed
     request including the GSB fetch. Remaining: the binding's HTTP login chain (prepare GET → login POST
     → relay GET, cookie capture) and the panel's credential fields. (This Phase-3 login is for the
     **account/GSB** leg; it is NOT required to reach the lobby VALIDATE — see the NW-S5 correction
     below.)
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
  NOVAWORLDUDP containers / `HandleClientHello` + `HandleClientJoin` (waves 1–2); the `ClientAuth.hk`
  echo (P1, landed); and the client GSB request (P2, NW-G3 — the `.gsb` URL is not a binary literal,
  so the client GETs a server-provided URL). All crypto and server-direction parsing are anchored to
  retail.
- **P3 witnessed (grill NW-S5/B, 2026-06-12; see `docs/net/novaworld-net-re.md` Wave 4):** the login
  submit is a **POST** of `application/x-www-form-urlencoded` to `NWLogin.dll`
  (`GopherWebWidget_SendHttpPost @ 0x658b30`) with EPASK-encrypted `NAME`/`PASSWORD` and a plaintext
  echoed `EPASK` bundle; the bundle arrives as a `Set-Cookie`; **and the shared HTTP client attaches
  every subnet-keyed cookie — including `NWHANDLE`/`PCID` — to the GSB fetch (the open question above is
  resolved: yes).** The Godot-free helpers now live in `libs/novaworld/http_login`.
- **NW-S5 CORRECTED by Wave 5 (2026-06-12; see `docs/net/novaworld-net-re.md` Wave 5).** A full
  capture of a *successful* retail session against the real `.204` refuted the Wave-4 inference
  that the verify-leg stall needed the HTTP login. Login is **not** a lobby-verify prerequisite
  (retail validates with placeholder udpcodes, before any HTTP), and the verify `Cookie` var-list
  is **CD-key/hardware identity with the CD-key fields empty** — not login cookies — so the lobby
  verify is **not credential-gated**. The actual stall was a DSP **seq/ack** bug in our client
  (0-based outbound seq + an un-acked settings packet). Fixed in `client_session.{h,cpp}`
  (1-based seq, header-only acks, full verify var-list with NWUID echoed). Oracle:
  `tests/novaworld/nw204_lobby_decode_test`.
- **Still to witness (client direction):** the host/join HTTP→UDP handoffs (P4/P5). Nothing is blocked
  on un-grilled crypto.

## Consequences

- Every leg is unit-testable before it ever touches a live server, because the protocol lives in
  `libs/` and the binding is a thin pump. This is the main reason for the `client_session` extraction.
- Phase 0 alone makes the client a switchable OpenNova/real-NovaWorld client at the gate+hello level;
  each later phase widens what works against both targets with no rework, because they share the same
  state machine.
- The work reuses the already-verified crypto and the existing builders/parsers, so the surface area
  of *new* code per leg is small (mostly state wiring + the client HTTP flows).
- Posture: **wire compatibility is always the design target** — our clients can join original
  servers, our servers can serve original clients, and opennova↔opennova works the same way. The
  caution is narrow and specific, and is about traffic to live infrastructure, not about how we write
  code: connecting a reimplemented client to NovaLogic's **live hosted** service is done sparingly,
  for parity testing, without load or abuse, and the retail executable remains the normal way to play
  on the official servers. opennova↔opennova remains our primary product path.

## Amendment (2026-06-22) — wire compatibility is a standing requirement, distinct from live-service traffic

The original Decision 2 / Posture wording read as if interop with original endpoints were only a
parity-testing artifact subordinate to opennova↔opennova. It is not. Wire compatibility — our clients
joining original servers, our servers serving original clients, opennova↔opennova on the same protocol
— is a first-class design requirement, the parity rule applied to the byte stream (root `CLAUDE.md`
Conventions; `docs/engine-primer.md` §3). The "sparingly / no load or abuse" caution is narrowed here
to its real subject: sending traffic to NovaLogic's *live hosted* service. That caution does not
license writing non-wire-compatible code.
