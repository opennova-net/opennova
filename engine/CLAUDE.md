# engine/ — the engine (portable C++ core)

- Godot-agnostic, strictly: no Godot/godot-cpp types or includes anywhere under `engine/`.
  Godot binding code lives only in `godot/src/`.
- Four groups (ADR 0028) — the directories and, since ADR 0029, the CMake build targets
  too; still never namespaces or include-path segments:
  - `base/` — shared substrate and repo plumbing: io, crt, vfs, resource_index,
    gameprofile, pcapio.
  - `formats/` — one library per NovaLogic format (ADR 0024; what earns a lib vs stays
    runtime-fused: ADR 0030), 31 today: adm, aip, pff, scr, sph, bfc1, pcx, fnt, rtxt,
    cbin, threedi, bad, def, avatars, mission, trn, cpt, til, foliage, env,
    mnu, mns, sbf, lwf, dbf, mus, playersav, particle (.ptl), score, bink,
    wac (front end; compiler/VM stay runtime).
  - `runtime/` — the in-match systems: world, wac (compiler/VM), mission (the runtime
    half — event runtime, promotion, boot; the document model is `formats/mission`),
    anim, audio, particle, renderer, controls, terrain, terrain_query,
    environment, hud, menu, simassets.
  - `net/` — the retail wire/protocol stack and portable in-match control
    (ADRs 0009–0012, 0019, 0036; Model-B-only): novacrypto, napi, npwire,
    novaworld, inmatch, plus internal netsim/npruntime implementation
    directories. `inmatch/session.*` owns lifecycle, role policy, fixed-tick
    banking, and input consumption over an `inmatch::TickTarget`; Godot orders
    its own presentation/device pipeline. `world::Match` owns gameplay rules,
    scoring, clocks, winner evaluation, and the frozen result. Wire code only
    serializes that result.
- Layout per library (FLAT since 2026-08-10): `engine/<group>/<domain>/*.{h,cpp}` —
  headers and sources sit side by side in the lib dir (nested subdirs allowed, e.g.
  `npwire/wire/`), and each GROUP directory is the one public include dir, so
  `#include <domain/file.h>` resolves to `engine/<group>/<domain>/file.h`. The
  one-directory-per-format principle, fixtures, and tests (ADR 0024) survive both
  ADR 0029's target collapse and the flatten; a library builds inside its group
  target, not as its own (see the group-targets bullet below). Two prefix notes from
  the flatten: `terrain_query` owns its own `<terrain_query/...>` prefix (the ADR
  0020 seam headers — pre-flatten they shared `terrain/`), and the .ptl lib lives at
  `engine/formats/particle` (its historical `<particle/...>` prefix names the dir).
  Namespace `opennova`. Native consumers link the static group targets directly;
  no shared-library/FFI export surface is maintained.
- Group targets (ADR 0029): FIVE STATIC targets, no per-lib ones (single ratified
  exception: the `opennova_crt` STATIC leaf under `base/crt` — the one mutable
  thread-local CRT rand stream; formats cannot link `opennova_base`, which sits
  above formats, so both `opennova_formats` and `opennova_runtime` PUBLIC-link the
  leaf directly) — `opennova_formats`
  (every formats/ lib; the mission FORMAT lib's membership here is the fold that keeps
  the four-group partition acyclic), `opennova_base` (vfs, resource_index, gameprofile,
  pcapio), `opennova_runtime` (the rest of runtime/), `opennova_net` (novacrypto,
  napi, npwire, inmatch, netsim, npruntime + novaworld session/gate), and
  `opennova_novaworld_service` (the service alone — the ONLY target linking
  `opennova_sqlite`; the Godot layer (`godot/src`) links `opennova_net`, never the
  service).
  `opennova_io` stays header-only INTERFACE. PUBLIC chain: formats
  links io, base links formats (base deliberately sits ABOVE formats because vfs
  parses pff/scr/bfc1),
  runtime links base, net links runtime, the service links net. The ADR 0024 family
  groups are deleted as subsumed. ADR 0020's terrain seam remains the four
  `<terrain_query/...>` headers: net, wac, mission, and world must not include the
  concrete `terrain/` implementation. The service target remains the sole sqlite
  consumer.
- Shared infrastructure lives in `engine/base/io` (`opennova::io` / `opennova::strutil`,
  header-only): bounds-checked `ByteReader`/`ByteWriter`, LSB-first `BitReader`/
  `BitWriter`, `io/le.h` primitives (including the `append_*_le` vector writers every
  streaming encoder wants), `io/fixed.h` (16.16 / 2.14), `io/log.h` (the diagnostic
  sink), `io/strutil.h` ASCII case-insensitive helpers. Do not hand-roll a new byte
  reader; migrate existing per-lib copies on-touch (delegate the
  body, keep the local signature, gated on that lib's byte-exact roundtrip tests).
- Two byte-cursor CONTRACTS exist on purpose, and a copy is only duplication if it
  matches one of them. `io::ByteReader` is the FORMAT-PARSER contract: a clipped read
  yields 0, the cursor does not advance, and parsing continues, so a file still
  round-trips byte-exactly; `ok()` reports truncation without changing that. A PROTOCOL
  decoder wants the opposite — the first short read poisons the cursor so a truncated
  datagram cannot half-decode into plausible state; that is
  `engine/net/npwire/wire_cursor.h`, and it must not be folded into `ByteReader`.
- Migration exceptions, each with its reason (do not "clean these up" casually):
  the `mus`/`wac` VM program-counter cursors are a witnessed faithful-port surface with
  their own clamp semantics. `engine/formats/cpt`'s bit codec and `io/bit_stream.h` have DIVERGED
  since the latter was lifted (cpt's writer carries a normalizing `set_position` and a
  `write_to_file`; its reader carries `remaining_bits`) — adopting the shared one in cpt
  is a real migration needing a CPT-corpus byte diff, not a swap. That byte diff is
  NOT in ctest today: `tests/cpt/cpt_roundtrip_test` (ctest `cpt_roundtrip`) pins the
  bit codec and the DPTH/CDEP/POLY round-trips on synthetic buffers only, so run a
  retail-corpus byte diff by hand whenever you touch the CPT encoder. The BMS `Reader`
  (`engine/formats/mission/bms.cpp` since the mission-format move) is still its own
  class with a safe bound (`count <= remaining()`), so what remains is a mechanical
  migration, not a hardening one.
- Ports are faithful structural translations of the original engine — implementing "our
  own version" of engine behavior is never allowed unless a tracked decision (ADR or an
  RE-record divergence entry) says otherwise. CRT/OS/platform primitives (strcpy/sprintf/
  memcpy, D3D, file I/O) are excluded — use standard equivalents. Cite the original inline
  at the port site: `[orig: Name @ 0xADDR]`. Engine-wide conventions: docs/engine-primer.md.
- Parity writers are built from scratch. Never smuggle raw input bytes through a writer to
  turn a parity test green (docs/adr/0003-no-raw-passthrough-create-from-scratch.md).
  The full writer-parity gate is writer from scratch + roundtrip test + retail-corpus
  byte sweep where a corpus exists + a ledgered D-entry when output legitimately differs.
- The protocol libs (`engine/net/novacrypto`, `engine/net/napi`, `engine/net/npwire`, `engine/net/novaworld`, `engine/net/netsim`) are
  held to wire compatibility: encoders produce bytes a stock client/server accepts,
  decoders read what a stock client/server emits, and opennova↔opennova requires
  encoder/decoder self-consistency. The witness record is docs/net/novaworld-net-re.md.
- Size ratchet: no `engine/`/`apps/`/`godot/src` `.cpp` past 2500 lines — split by leg
  into a sibling TU first (precedent: `world/infantry.cpp` -> `infantry_ladder.cpp`),
  never bump the baseline.
- Tests for this code live in `/tests/<domain>/` (ctest), not `godot/tests/`.
- 3DI models: 3DI3 only, consumed directly (ADR 0027). `threedi_3di3_read` produces
  `Threedi3di3` (engine/formats/threedi/threedi_3di3.h) and that parsed struct IS
  the model every runtime consumer walks; the native parity writer accepts the same
  struct. There is no
  intermediate model representation, and the GP-era (GPM/GPS/GPP) reader/writer is gone —
  the format knowledge lives in docs/threedi/3di-gp-format-re.md. Shared derivations are
  3DI3-native helpers in that header (userpoint decode, collision run prefix sums +
  runtime-safety validation, `threedi_3di3_ground_anchor`); load-time fixups (the CFAC
  normal-run resolve) happen at the consumer, where retail's loader performs them.
