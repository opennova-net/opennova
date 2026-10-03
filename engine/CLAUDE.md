# engine/ — the engine (portable C++ core)

- Godot-agnostic, strictly: no Godot/godot-cpp types or includes anywhere under `engine/`.
  Godot binding code lives only in `godot/src/`.
- Four groups (ADR 0028) — the directories, since ADR 0029 the CMake build targets,
  and since ADR 0040 the first include-path segment too (never C++ namespaces):
  - `base/` — shared substrate and repo plumbing.
  - `formats/` — one library per NovaLogic format (ADR 0024; what earns a lib vs stays
    runtime-fused: ADR 0030). `formats/wac` is the bytecode/program model, the command
    table and help; the compiler and VM are runtime.
  - `runtime/` — the in-match systems: world, wac (compiler/VM), mission (the runtime
    half — event runtime, promotion, boot; the document model is `formats/mission`),
    anim, audio, particle, renderer, controls, terrain, terrain_query,
    environment, hud, menu, assets, and devtools (the Dear ImGui pass, ADR
    0039: infrastructure like io/vfs, not a port, so it sits in the citation
    allowlist; the frame-stats board builds in every flavour, while Dear ImGui,
    the pass and the game's F3 windows build only with `OPENNOVA_DEVTOOLS` — off for
    the release GDExtension flavour and the web build; ImGui headers never leave
    the group, the shell hands the context over as plain pointers via
    `devtools/imgui_abi.h`).
  - `net/` — the retail WIRE (ADRs 0009–0012, 0019; ADR 0043 d4: net means
    wire): novacrypto, napi, npwire (the in-game codec, the NWU session
    framing, capture decode, the LAN discovery codec, the datagram-socket
    seam) and novaworld (the session/gate legs + the service). Nothing under
    `net/` includes `runtime/`; `opennova_net` never links `opennova_runtime`.
  - the in-match control lives in `runtime/` (ADR 0043 d4): `runtime/inmatch`
    (ex net/inmatch + net/npruntime: `session.*` owns lifecycle, role policy,
    fixed-tick banking and input consumption over a `Role`; the
    listen-host frame; the IDA-faithful server/client state machines and
    frame loops; the transports) and `runtime/replication` (ex net/netsim:
    the world<->wire seam, the client replica state and its folds). Godot
    orders its own presentation/device pipeline. `world::Match` owns gameplay
    rules, scoring, clocks, winner evaluation, and the frozen result; wire
    code only serializes that result.
- Layout per library (FLAT since 2026-08-10): `engine/<group>/<domain>/*.{h,cpp}` —
  headers and sources sit side by side in the lib dir (nested subdirs allowed, e.g.
  `npwire/wire/`), and `engine/` is the ONE public include root (ADR 0040): every
  engine header is included as `#include <group/domain/file.h>` —
  `<runtime/world/player_view.h>`, `<formats/mission/mission.h>`, `<base/vfs/vfs.h>`,
  `<net/npwire/peer_addr.h>` — from engine, apps, tests and godot/src alike; only a
  same-directory sibling may use a bare `"file.h"`. The group in the path is what
  keeps `formats/mission` and `runtime/mission` (likewise `particle`, `wac`) apart
  and makes the layering a lint (`scripts/lint/include_graph_check.py`: a tree
  includes only the groups below it, no unqualified engine include, the ADR 0020
  terrain seam, no `godot` include under engine/apps/tests). The
  one-directory-per-format principle, fixtures, and tests (ADR 0024) survive the
  target collapse, the flatten and the root change; a library builds inside its
  group target, not as its own (see the group-targets bullet below).
  Namespace `opennova`. Native consumers link the static group targets directly;
  no shared-library/FFI export surface is maintained.
- Language: C++17 throughout (ADR 0043 d14). `engine/` carries no `.c` source
  and no `extern "C"`; the vendored C (miniz, bcrypt, sqlite) lives under
  `third_party/`. The format libs that were C APIs keep their C NAMES inside
  `namespace opennova::<lib>` — `opennova::pff::pff_open`,
  `opennova::def::DefWeaponDef`, `opennova::threedi::Threedi3di3`,
  `opennova::gameprofile::gameprofile_by_code`, `opennova::crt::crt_rand15` — and
  the former `DEF_*` / `PFF_*` / `FNT_*` / `THREEDI_*` macros are
  `inline constexpr` of the same names (typed by their literal: `uint32_t` for
  the u-suffixed masks and magics, `int` otherwise). A consumer `.cpp` takes
  `using namespace opennova::<lib>;` after its includes; a consumer HEADER
  spells the qualified name (a forward declaration goes inside the lib's
  namespace, never bare). Every header is `#pragma once`. The whitespace
  style is `.clang-format` at the repo root (tabs, 4-wide, 100 columns,
  `NamespaceIndentation: None`) — config only until the whitespace-only
  reformat commits land; never reformat a file as part of another change.
- Web-portable (ADR 0049 d5): the web build links this code into a wasm32 side module
  whose templates abort on any throw, so nothing uses exceptions as control flow
  (`strutil::parse_int` / `parse_ulong` / `parse_float`, never `try { std::stoi }`);
  thread counts are the embedder's (the terrain composer's `Threads` budget,
  `Threads::for_hardware()` being the desktop sizing); layout guards hold on ILP32.
- Group targets (ADR 0029): FIVE STATIC targets, no per-lib ones (single ratified
  exception: the `opennova_crt` STATIC leaf under `base/crt` — the one mutable
  thread-local CRT rand stream; formats cannot link `opennova_base`, which sits
  above formats, so both `opennova_formats` and `opennova_runtime` PUBLIC-link the
  leaf directly) — `opennova_formats`
  (every formats/ lib; the mission FORMAT lib's membership here is the fold that keeps
  the four-group partition acyclic), `opennova_base` (vfs, resource_index, gameprofile,
  pcapio), `opennova_net` (novacrypto, napi, npwire + novaworld session/gate — the
  wire), `opennova_runtime` (the rest of runtime/, including `inmatch` and
  `replication`), and `opennova_novaworld_service` (the service alone — the ONLY
  target linking `opennova_sqlite`; the Godot layer (`godot/src`) links
  `opennova_runtime`, which PUBLIC-links `opennova_net`, never the service).
  `opennova_io` stays header-only INTERFACE. PUBLIC chain (ADR 0043 d4): formats
  links io, base links formats (base deliberately sits ABOVE formats because vfs
  parses pff/scr/bfc1), net links base, runtime links net, the service links net;
  `link_graph_check.py` forbids `opennova_net -> opennova_runtime` and keeps the
  sqlite containment. The ADR 0024 family groups are deleted as subsumed; ADR
  0020's terrain seam is include-level (`scripts/lint/include_graph_check.py` —
  for inmatch/replication/wac/mission/world the `runtime/terrain/` prefix is fully
  forbidden; the seam is terrain_query's `<runtime/terrain_query/...>` headers),
  and every runtime lib but inmatch/replication is NET-AGNOSTIC (no `net/`,
  `runtime/inmatch/` or `runtime/replication/` include — the same lint).
- Shared infrastructure lives in `engine/base/io` (`opennova::io` / `opennova::strutil`,
  header-only): bounds-checked `ByteReader`/`ByteWriter`, LSB-first `BitReader`,
  `io/le.h` primitives (including the `append_*_le` vector writers every
  streaming encoder wants), `io/fixed.h` (16.16 / 2.14), `io/log.h` (the diagnostic
  sink), `io/strutil.h` ASCII case-insensitive helpers, `io/os_path.h` (a UTF-8 path
  string at an OS file call: `os_path`, `fopen_utf8`, `utf8_path`; Windows reads a
  narrow path in the ANSI code page and fails one past MAX_PATH without `\\?\`). Do not hand-roll a new byte
  reader; migrate existing per-lib copies on-touch (delegate the
  body, keep the local signature, gated on that lib's byte-exact roundtrip tests).
  The 16.16 / 2.14 scales are `io/fixed.h`'s `kFp16One` (float), `kFp16OneD`
  (double), `kFp16OneInt`, `kInvFp16One` and `kFp14One`, and the logic clock is
  `io/tick_rate.h`'s `kTickHz` (62.5) / `kTicksPerSecondInt` (62): name a raw
  65536 / 16384 / 62 on touch with the constant of the SAME type (a float divide
  and a double divide round differently; a cited line keeps its literal spelling).
- Two byte-cursor CONTRACTS exist on purpose, and a copy is only duplication if it
  matches one of them. `io::ByteReader` is the FORMAT-PARSER contract: a clipped read
  yields 0, the cursor does not advance, and parsing continues, so a file still
  round-trips byte-exactly; `ok()` reports truncation without changing that. A PROTOCOL
  decoder wants the opposite — the first short read poisons the cursor so a truncated
  datagram cannot half-decode into plausible state; that is
  `engine/net/npwire/wire_cursor.h`, and it must not be folded into `ByteReader`.
- Migration exceptions, each with its reason (do not "clean these up" casually):
  the `mus`/`wac` VM program-counter cursors are a witnessed faithful-port surface with
  their own clamp semantics. `engine/formats/cpt` reads through the shared
  `io::BitReader` but keeps its own bit WRITER (a normalizing `set_position` and a
  `write_to_file`); `io/bit_stream.h` carries no writer, and replacing cpt's is a real
  migration needing a CPT-corpus byte diff, not a swap. That byte diff is
  NOT in ctest today: `tests/cpt/cpt_roundtrip_test` (ctest `cpt_roundtrip`) pins the
  bit codec and the DPTH/CDEP/POLY round-trips on synthetic buffers only, so run a
  retail-corpus byte diff by hand whenever you touch the CPT encoder. Three more stay
  by design: `formats/bink`'s
  `BitReader` is a fail-latching decoder contract (`peek`, `align32`, the first short
  read poisons it), the `wire_cursor` posture rather than `io::BitReader`'s lenient
  zero-fill; `net/npwire/wire/ingame_encode.cpp`'s `Writer` already rides
  `io::append_*_le` and exists only for the witnessed `cstr`/`cstr_capped` wire string
  forms over an external buffer; `formats/bad` and `formats/rtxt` read through
  offset-indexed wrappers over `io::read_*_le` plus a NUL-string scanner, not cursors.
- Ports are faithful structural translations of the original engine — implementing "our
  own version" of engine behavior is never allowed unless a tracked decision (ADR or an
  RE-record divergence entry) says otherwise. CRT/OS/platform primitives (strcpy/sprintf/
  memcpy, D3D, file I/O) are excluded — use standard equivalents. Cite the original inline
  at the port site: `[orig: Name @ 0xADDR]`. Engine-wide conventions: docs/engine-primer.md.
- Parity writers are built from scratch. Never smuggle raw input bytes through a writer to
  turn a parity test green (docs/adr/0003-no-raw-passthrough-create-from-scratch.md).
  The full writer-parity gate is writer from scratch + roundtrip test + retail-corpus
  byte sweep where a corpus exists + a ledgered D-entry when output legitimately differs.
- The protocol libs (`engine/net/novacrypto`, `engine/net/napi`, `engine/net/npwire`, `engine/net/novaworld`) and the
  replication seam (`engine/runtime/replication`) are
  held to wire compatibility: encoders produce bytes a stock client/server accepts,
  decoders read what a stock client/server emits, and opennova↔opennova requires
  encoder/decoder self-consistency. The witness record is docs/net/novaworld-net-re.md.
- Size ratchet: no `engine/`/`apps/`/`godot/src` `.cpp` or `.h` past 2500 lines — split
  by RESPONSIBILITY (one type per TU pair: the ladder climb, the combat pass, the remote
  anim are responsibilities; "the tail of tick_infantry" is not), never by leg; a TU
  under ~100 lines with one owner folds back into it (ADR 0043). Never bump the baseline.
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
