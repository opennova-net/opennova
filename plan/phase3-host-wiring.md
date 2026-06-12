# Phase 3 host wiring — the remaining EPASK login work

> The Godot-free protocol core landed (`libs/novaworld/http_login.{h,cpp}` +
> `tests/novaworld/http_login_test`). This file tracks the host-side wiring that
> remains, plus the product decisions it needs. Fluid — update as it lands.

## What's done

- Wire contract witnessed end to end (grill NW-S5/B; `docs/net/novaworld-net-re.md` Wave 4).
- `build_credentials_post_body`, `parse_set_cookie_values`, `CookieJar` — verified against
  the server's own `parse_form_body` + EPASK decode by ctest `http_login`.
- Server endpoints already exist (`/nwprepare.dll`, `/NWStart.dll`, `POST /NWLogin.dll`,
  `GET /NWLogin.dll` relay) with real EPASK auth against the seeded users.

## What remains (binding + UI)

The binding is `godot/engine/network/nova_world_client.cpp`. The HTTP transport is a Godot
`HTTPRequest` child (the GSB browse leg already uses one). Login adds an HTTP chain that, for
the **real-NW target**, must run **before** the gate probe — NW-S3 established that the gate
issues `UDPCODE1/2` only to an authenticated (cookie-bearing) request, and NW-S5 that the
verify leg needs the login `Cookie` var-list. For the **OpenNova target** the server is
permissive, so login is optional there.

1. **Login chain** (new state(s) before `GATE_PROBING` when a credential is set):
   - GET `<post_host>/nwprepare.dll?...` → capture the `EPASK` Set-Cookie into a `CookieJar`.
   - POST `<post_host>/NWLogin.dll` with `build_credentials_post_body(epask, name, password, hidden)`
     and `Content-Type: application/x-www-form-urlencoded` → capture `LOGINSESSIONTAG` + the
     relay refresh URL.
   - GET the relay (`<post_host>/NWLogin.dll` with the `LOGINSESSIONTAG` cookie) → capture
     `NWHANDLE`/`PCID`/`EXPBITS`.
   - Attach `jar.cookie_header()` to the gate probe, the GSB fetch, and any host/join request
     (the engine attaches them to *every* subnet request — NW-S5/B3).
2. **Verify leg:** thread the jar into `ClientSession` so `ClientRequestVerifyResult` carries the
   `Cookie` var-list (and consider the timing fix: emit `ClientConnected` after the session
   settles rather than on `ServerAuth`). Needed only for the real-NW target.
3. **Panel UI** (`godot/game/novaworld_panel.gd`): username + password fields, a Login button,
   and login state in the status line. Today the panel is browse-only.
4. **Settings** (`godot/game/novaworld_settings.gd`): self-described growth point for login.

## Decisions for the user (shape the wiring; not yet made)

- **Credential storage.** Options: (a) session-only, nothing persisted (simplest, safest);
  (b) remember username only (in the existing `user://novaworld_client.cfg`), password each
  session; (c) remember both via an encrypted store (retail uses `PERSISTENTREMEMBERLOGINDATA`,
  EPASK-encrypted under a fixed key — NW-C2). Recommendation: start at (b).
- **Login policy per target.** Real-NW requires login (it gates AUTH). For the OpenNova target:
  offer optional login (dev-seed fallback as today) vs require it. Recommendation: optional for
  OpenNova, required for real-NW.
- **Live verification.** The real-NW path can only be confirmed with a real NovaWorld account
  against `gs.novaworld.net`; that run is the user's (we do not impersonate at load). The
  OpenNova path is verifiable locally end to end.
