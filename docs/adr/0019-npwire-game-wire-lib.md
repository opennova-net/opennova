# ADR 0019: libs/npwire — the game wire protocol library

- **Status**: accepted (2026-07-04, rides the NET-2 extraction PR)
- **Owners**: maturity program NET track
- **Supersedes/updates**: refines the lib topology around ADR 0013's
  matchmaking/in-match split; path citations in earlier ADRs (0009–0013)
  describe the pre-move layout and are left as written.

## Context

The in-match *runtime* already lives outside the NovaWorld lib
(`libs/netsim`, `libs/npruntime`), but the in-match *codec* (wire leg),
the capture/replay chain, and the NWU session framing sat under
`libs/novaworld` — a matchmaking name for game-protocol code
(maturity umbrella, NET track background). Consumers that never touch
matchmaking (netsim, npruntime, nw_pp, nw_replay, the Godot client)
were linking into a lib whose name says otherwise, and the future
world→terrain seam work (LIBS-1) wants link topology to churn once.

`libs/novaworld` was already structured as five leg sub-targets
(wire / session / gate / service / replay) behind an INTERFACE umbrella,
so the physical cut follows existing seams.

## Decision

1. **Name**: the extracted library is **`libs/npwire`** (NP = the NAPI
   "NovaLogic Protocol" family; wire = what it is). One CMake target,
   `opennova_npwire`, per the libs/ convention — the internal leg split
   was an artifact of novaworld's mixed concerns and is not preserved.
2. **Contents**: the wire leg whole (`ingame_decode/encode`,
   `ingame_message_catalog.h`, `replication_model.h`, `peer_addr.h`),
   the replay leg whole (`wire_capture`, `replay_timeline`,
   `serverlog_decode`), and the session-framing half of the session leg
   (`protocol_message`, `nw_session_framing`, `session_hello`,
   `session_keys`). Headers stay flat under `include/npwire/`; sources
   keep their leg grouping under `src/`.
3. **Direction**: `novaworld → npwire → napi/novacrypto`. Matchmaking
   sits ON the wire base, never the reverse. The extraction carried
   zero backward includes (verified pre-move); the residual novaworld
   session and service legs now link `opennova_npwire`
   (`client_session.h` uses the framing; `connection/registry.h` uses
   `peer_addr.h`).
4. **`session_protocol` stays in novaworld** (the one judgment call):
   `classify_session_protocol` is a leaf PN classifier used by session
   establishment above the framing. Moving it would not free any
   consumer of its novaworld link (npruntime also needs
   `client_session.h`). Revisit only if a future slice makes npruntime
   novaworld-free.
5. **C ABI**: npwire is C++-linked only — it is NOT part of the flat
   C ABI (`opennova_shared`), same stance as every net lib. NET-4 landed
   the guard (2026-07-05): the `abi_export_identity` ctest
   (`scripts/lint/abi_exports_check.py` + the committed
   `abi_exports_baseline.txt`) pins the flat export list and hard-fails
   on any net-family symbol, mangled or flat — the forbidden check is
   not bypassable by a baseline bump.
6. **Wire compatibility** (the standing invariant) transfers with the
   code: npwire joins the protocol-lib list held to it
   (libs/CLAUDE.md); the witness record remains
   docs/net/novaworld-net-re.md.

## Consequences

- `libs/novaworld` is now honestly the matchmaking/service lib: session
  state above the framing, gate, service/persistence (the only sqlite
  link), behind the same `opennova_novaworld` umbrella.
- Consumers of moved headers rewrite `<novaworld/X.h>` → `<npwire/X.h>`;
  pure-wire consumers (netsim, nw_pp, nw_replay) drop their novaworld
  link entirely.
- npwire is registered in both CMake roots (the top-level build and
  `godot/engine`'s standalone tree); `apps/common` links it explicitly
  for `peer_addr.h` instead of free-riding on transitive include dirs.
- CI: the `novaworld-images.yml` `paths:` filters gain `libs/npwire/**`
  and its http-tests job gains `nw_codec_identity_test` (parity with
  ci.yml's Linux net job, which #188 already synced).
- Tests keep their names and homes (`tests/novaworld/`,
  `tests/npruntime/`, `tests/netsim/`); they build via the umbrella, so
  the move is invisible to their link lines. A later slice may re-home
  purely-wire tests under `tests/npwire/`; not this one (pure move).
- Godot GDScript "keep in sync" comments and the living witness docs
  (correspondence.md, novaworld-net-re.md, engine-primer.md) now cite
  `libs/npwire` paths for the moved pieces.
