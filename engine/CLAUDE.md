# engine/ — the engine (portable C++ core)

- Godot-agnostic, strictly: no Godot/godot-cpp types or includes anywhere under `engine/`.
  Godot binding code lives only in `godot/src/`.
- Five groups (ADR 0028, ADR 0046 d3): the directories, the CMake build targets (ADR 0029)
  and the first include-path segment (ADR 0040), never C++ namespaces:
  - `base/` — shared substrate and repo plumbing.
  - `formats/` — one library per NovaLogic format (ADR 0024; what earns a lib vs stays
    runtime-fused: ADR 0030). `formats/wac` is the bytecode/program model, the command
    table and help; the compiler and VM are runtime.
  - `runtime/` — the in-match systems: world, wac (compiler/VM), mission (the runtime
    half — event runtime, promotion, boot; the document model is `formats/mission`),
    anim, audio, particle, renderer, controls, terrain, terrain_query,
    environment, hud, menu, assets, and devtools (the Dear ImGui pass, ADR
    0039: infrastructure like io/vfs, not a port, so it sits in the citation
    allowlist; the frame-stats board builds in every flavour; Dear ImGui, the pass
    and the tool kit both products' windows draw with (`devtools/ui_kit.h`) build with
    `OPENNOVA_IMGUI` (either product), the game's F3 windows only with
    `OPENNOVA_DEVTOOLS` — off for the release GDExtension flavour;
    ImGui headers stay in the group and `editor/ui`, the shell hands the context over
    as plain pointers via `devtools/imgui_abi.h`).
  - `net/` — the retail WIRE (ADRs 0009–0012, 0019; ADR 0043 d4: net means
    wire): novacrypto, napi, npwire (the in-game codec, the NWU session
    framing, capture decode, the LAN discovery codec, the datagram-socket
    seam), novaworld (the session/gate legs + the service) and admin (the
    game server's remote-admin TCP protocol). Nothing under
    `net/` includes `runtime/`; `opennova_net` never links `opennova_runtime`.
  - the in-match control lives in `runtime/` (ADR 0043 d4): `runtime/inmatch`
    (`session.*` owns lifecycle, role policy, fixed-tick banking and input consumption
    over a `Role`; the listen-host frame; the IDA-faithful server/client state machines
    and frame loops; the transports) and `runtime/replication` (the world<->wire seam,
    the client replica state and its folds). Godot
    orders its own presentation/device pipeline. `world::Match` owns gameplay
    rules, scoring, clocks, winner evaluation, and the frozen result; wire
    code only serializes that result.
  - `editor/`: the OpenNova Editor's portable core (ADR 0046 d3); its libraries are
    mapped in [`editor/README.md`](editor/README.md). STATIC `opennova_editor`,
    PUBLIC-linking `opennova_runtime`; nothing under the four groups above may include or
    link it (`include_graph_check.py`, `link_graph_check.py`), and the game and the Play
    child never carry it. Five of its libraries are ranked, model < documents < graph <
    session < ui (`include_graph_check.py`'s editor rank): a ranked library includes only
    its own rank and below, `graph/reference_kinds.h` being a seam header any may include;
    an upward include is a listed lint exception naming the slice that removes it, and an
    exception the tree no longer makes fails the lint. The other editor libraries stay
    unranked. Only `editor/ui` and `runtime/devtools` may include an ImGui header. Tooling,
    not a port: its sources sit in the citation allowlist by the `editor/` prefix. No
    directory under it may start with `build` (the lints skip such directories; the build
    lib is `project_build/`).
- Layout per library: `engine/<group>/<domain>/*.{h,cpp}`,
  headers and sources sit side by side in the lib dir (nested subdirs allowed, e.g.
  `npwire/wire/`), and `engine/` is the ONE public include root (ADR 0040): every
  engine header is included as `#include <group/domain/file.h>` —
  `<runtime/world/player_view.h>`, `<formats/mission/mission.h>`, `<base/vfs/vfs.h>`,
  `<net/npwire/peer_addr.h>` — from engine, apps, tests and godot/src alike; only a
  same-directory sibling may use a bare `"file.h"`. The group in the path is what
  keeps `formats/mission` and `runtime/mission` (likewise `particle`, `wac`) apart
  and makes the layering a lint (`scripts/lint/include_graph_check.py`: a tree
  includes only the groups below it, no unqualified engine include, the ADR 0020
  terrain seam, no `godot` include under engine/apps/tests). One directory per format,
  with its fixtures and tests (ADR 0024); a library builds inside its group target, not
  as its own (the group-targets bullet below).
  Namespace `opennova`. Native consumers link the static group targets directly;
  no shared-library/FFI export surface is maintained.
- Language: C++17 throughout (ADR 0043 d14). `engine/` carries no `.c` source
  and no `extern "C"`; the vendored C (miniz, bcrypt) lives under
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
  `NamespaceIndentation: None`); the tree is not reformatted wholesale, so never
  reformat a file as part of another change.
- Portable (ADR 0049 d5, kept by ADR 0062, which deleted the web build): nothing uses
  exceptions as control flow (`strutil::parse_int` / `parse_ulong` / `parse_float`, never
  `try { std::stoi }`); thread counts are the embedder's (the terrain composer's `Threads`
  budget and the TrnGen bake's `TerrainBakeInput::threads`, each `for_hardware()` being the
  desktop sizing); layout guards hold on ILP32. No CI job builds an ILP32 or no-exceptions
  target, so review keeps these. The NovaWorld service (`opennova_novaworld_service`,
  Linux only, never part of a game build) is outside this rule: its database layer
  reports a failed query or migration by throwing.
- Group targets (ADR 0029, ADR 0046): six STATIC group targets, no per-lib ones:
  `opennova_formats` (every formats/ lib, the mission FORMAT lib included, which keeps the
  partition acyclic), `opennova_base` (vfs, resource_index, gameprofile, pcapio, os_random,
  proc), `opennova_net` (novacrypto, napi, npwire, admin and the novaworld session/gate: the
  wire), `opennova_runtime` (the rest of runtime/, `inmatch` and `replication` included),
  `opennova_novaworld_service` (the service alone, only with the root option
  `OPENNOVA_NOVAWORLD_SERVICE`, the ONLY target linking `opennova_pq`, the INTERFACE target
  over the system libpq) and `opennova_editor` (linked only by the editor-enabled
  GDExtension variant, `apps/project` and the tests). The one ratified per-lib exception is
  the `opennova_crt` STATIC leaf under `base/crt` (the one mutable thread-local CRT rand
  stream), which `opennova_formats` and `opennova_runtime` both PUBLIC-link, since formats
  cannot link base. `opennova_io` stays header-only INTERFACE. The PUBLIC chain (ADR 0043
  d4): formats links io, base links formats (base sits ABOVE formats because vfs parses
  pff/scr/bfc1), net links base, runtime links net, the service links net; the Godot layer
  links `opennova_runtime`, never the service. `link_graph_check.py` forbids
  `opennova_net -> opennova_runtime` and keeps the libpq containment. ADR 0020's terrain
  seam is include-level (`include_graph_check.py`: for inmatch/replication/wac/mission/world
  the `runtime/terrain/` prefix is forbidden; the seam is `<runtime/terrain_query/...>`),
  and every runtime lib but inmatch/replication is NET-AGNOSTIC (no `net/`,
  `runtime/inmatch/` or `runtime/replication/` include, the same lint).
- Shared infrastructure lives in `engine/base/io` (`opennova::io` / `opennova::strutil`,
  header-only): bounds-checked `ByteReader`/`ByteWriter`, LSB-first `BitReader`/`BitWriter`,
  `io/le.h` primitives (including the `append_*_le` vector writers every
  streaming encoder wants), `io/fixed.h` (16.16 / 2.14), `io/log.h` (the diagnostic
  sink), `io/strutil.h` ASCII case-insensitive helpers, `io/os_path.h` (a UTF-8 path
  string at an OS file call: `os_path`, `fopen_utf8`, `utf8_path`; Windows reads a
  narrow path in the ANSI code page and fails one past MAX_PATH without `\\?\`; and
  `fseek64` / `ftell64`, the stdio seek at 64 bits, since Windows' `fseek` takes a 32-bit
  `long`: every seek into an archive or a sound bank goes through it, D-VFS-16),
  `io/file_io.h` (whole-file reads and the atomic `.tmp`-then-rename write over
  `os_path`, the rename's bounded retry), `io/sha256.h` (FIPS 180-4 SHA-256: the
  streaming `Sha256` and the one-shot `sha256` / `sha256_hex`; infrastructure, not a
  port). A secret or one-of-a-kind value (a salt, a session tag or key, an id, a UUID)
  draws from `base/os_random` (the OS CSPRNG: `os_random_bytes`, `os_random_u32` /
  `os_random_u64` / `os_random_nonzero_u32`, `make_uuid_v4`, the `OsRandom` generator),
  never `std::random_device`, which libstdc++ serves from RDSEED (AMD's erratum answers
  concurrent draws with 0); it is a compiled lib in `opennova_base`, not `io`, because
  its Windows call needs `<windows.h>`. A child process starts through `base/proc`
  (`proc::ChildProcess`: an argv array with no shell, the identity, `process_liveness`,
  release without a kill), the one process seam of the apps and the front ends; it is
  platform plumbing (citation allowlist). The one `std::random_device` left in `engine/`
  is the narrow exception: the CBIN writer's fresh XOR key (`formats/cbin`, a credits
  file with no preserved key) is written in the clear in the file's own header, so it
  only obfuscates and is neither a secret nor an id, and formats cannot link base. The
  second named exception is the game protocol's wire-parity values, which retail derives
  from the NAPI manager's 16-bit LCG or the protocol's connection counter: the host key
  (HK), each connection's SK / CK and SCRK, and the CI (`net/npwire/nw_session_framing.h`:
  `manager_session_key`, `manager_connection_keys`, `next_connection_index`; D-NET-524,
  D-NET-521). They stay on the LCG and the counter by the maintainer's decision ("retail
  LCG everywhere"), predictable as retail's are (net-re §8, D-NET-524 records the cost):
  never move them back to the CSPRNG. Everything else stays on
  `os_random`: the NovaWorld service's values (HOSTKEY, the service's SK / SCRK, cookies,
  salts, tags and ids) and the NovaWorld lobby client's (its CI, CK and SCRK; D-NET-704
  tracks retail's LCG there and waits on the maintainer). Do
  not hand-roll a new byte reader; migrate existing per-lib copies on-touch
  (delegate the body, keep the local signature, gated on that lib's byte-exact roundtrip tests).
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
- Migration exceptions, each with its reason (do not "clean these up" casually): the
  `mus`/`wac` VM program-counter cursors are a witnessed faithful-port surface with their own
  clamp semantics; `formats/bink`'s `BitReader` is a fail-latching decoder contract (`peek`,
  `align32`, the first short read poisons it), the `wire_cursor` posture rather than
  `io::BitReader`'s lenient zero-fill; `net/npwire/wire/ingame_encode.cpp`'s `Writer` rides
  `io::append_*_le` and exists only for the witnessed `cstr`/`cstr_capped` wire string forms
  over an external buffer; `formats/bad` and `formats/rtxt` read through offset-indexed
  wrappers over `io::read_*_le` plus a NUL-string scanner, not cursors.
- The CPT codec (`engine/formats/cpt`) reads and writes through `io::BitReader` /
  `io::BitWriter`, its writer's bytes (the high-water rule, the zero pads) pinned by the
  retail corpus: run `--suite retail` (ctest `cpt_jo_assets_sweep`, every retail .cpt read and
  written again byte for byte) whenever you touch the CPT encoder; `cpt_roundtrip` and
  `cpt_trngen_bake` pin the bit codec and the bake in core.
- The parity rules (faithful ports cited `[orig: Name @ 0xADDR]`, writers from scratch, ADR
  0003) are the root CLAUDE.md's Conventions and docs/engine-primer.md. The full
  writer-parity gate is writer from scratch + roundtrip test + retail-corpus byte sweep
  where a corpus exists + a ledgered D-entry when output legitimately differs.
- The protocol libs (`engine/net/novacrypto`, `engine/net/napi`, `engine/net/npwire`, `engine/net/novaworld`,
  `engine/net/admin`) and the
  replication seam (`engine/runtime/replication`) are
  held to wire compatibility: encoders produce bytes a stock client/server accepts,
  decoders read what a stock client/server emits, and opennova↔opennova requires
  encoder/decoder self-consistency. The witness record is docs/net/novaworld-net-re.md.
- Size ratchet: no `engine/`/`apps/`/`godot/src` `.cpp` or `.h` past 2500 lines — split
  by RESPONSIBILITY (one type per TU pair: the ladder climb, the combat pass, the remote
  anim are responsibilities; "the tail of tick_infantry" is not), never by leg; a TU
  under ~100 lines with one owner folds back into it (ADR 0043). Never bump the baseline.
  Exception: in `engine/editor`, a document type's, a view's, a viewport view's or a session
  operation's own TU pair stays its own whatever its size, its one includer the registry or
  owner that lists it (`document_types.cpp`, `ui/document_views.cpp`, `ui/viewport_views.cpp`;
  the operations' `session/session_core.cpp` and `session/rename_controller.cpp`); one type
  per TU pair is how the editor is laid out.
- Tests for this code live in `/tests/<domain>/` (ctest), not `godot/tests/`.
- 3DI models: 3DI3 only, consumed directly (ADR 0027). `threedi_3di3_read` produces
  `Threedi3di3` (engine/formats/threedi/threedi_3di3.h) and that parsed struct IS
  the model every runtime consumer walks; the native parity writer accepts the same
  struct. There is no
  intermediate model representation. A BHD-era GP model (GPM/GPS/GPP) is read only to be
  migrated to 3DI3 (`engine/formats/threedi_gp`: `threedi_gp.h` the reader,
  `threedi_gp_migrate.h` the one-way migration `opennova-3di migrate` and the editor's
  import run; nothing writes GP, nothing at runtime walks it; ADR 0027 as amended,
  docs/threedi/3di-gp-format-re.md §5). Shared derivations are
  3DI3-native helpers in that header (userpoint decode, collision run prefix sums +
  runtime-safety validation, `threedi_3di3_ground_anchor`); load-time fixups (the CFAC
  normal-run resolve) happen at the consumer, where retail's loader performs them.
